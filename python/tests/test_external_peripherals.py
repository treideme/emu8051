"""unittest coverage for sim/devices/servo.c, sim/devices/enc28j60.c, and
sim/devices/fan.c -- peripherals that are NOT part of the HC6800-ES board
(see sim/README.md's "plugin vs board definition" note), so unlike
test_hc6800_es.py's coverage these are enabled with an explicit pin, not
a zero-argument enable_*().

Both need real compiled firmware to drive their pins as outputs (a test
can't synthesize an output pulse via Simulator.set_pin() -- that overrides
what a *read* sees, it does not trigger a device's write-side callback,
which only real executed instructions do), so both are still built from
the sibling demo repo and skipped cleanly if it isn't there.

Run with: python -m unittest discover -s python/tests -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from pysim import Simulator  # noqa: E402

STAGING_BUILD = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "stc89c52-staging", "build"
)


def hexpath(name):
    return os.path.join(STAGING_BUILD, name)


def skip_unless_built(name):
    path = hexpath(name)
    return unittest.skipUnless(
        os.path.isfile(path),
        f"{name} not found -- build the sibling demo repo first (meson setup build && ninja -C build)",
    )


class ServoTests(unittest.TestCase):
    @skip_unless_built("05_enc_servo.hex")
    def test_home_position_centers_regardless_of_encoder_count(self):
        # doc/blog-topics.md idea #12's "home-position detection": pressing
        # SW should drive the servo to a fixed center pulse (1500us, 90
        # degrees) regardless of wherever the encoder count happened to be.
        with Simulator("hc6800_es", hexpath("05_enc_servo.hex")) as sim:
            sim.enable_servo(3, 7, 1000, 2000)  # P3.7, assumed wiring -- see 05_enc_servo/enc.c
            sim.step_instructions(50_000)

            sim.set_pin(1, 5, 0)  # SW=0 (P1.5, active-low per enc.c)
            sim.step_instructions(30_000)
            # Generous deltas: real interrupt-response latency (see
            # doc/blog-topics.md idea #12's own "3-8 MC of jitter" note,
            # plus SDCC's own ISR prologue/epilogue) measured empirically
            # at ~30-55us here, and angle_decidegrees amplifies a
            # microsecond error by 1.8x (1800 decidegrees / 1000us span).
            self.assertAlmostEqual(sim.servo_pulse_us(), 1500, delta=60)
            self.assertAlmostEqual(sim.servo_angle_decidegrees(), 900, delta=110)
            sim.set_pin(1, 5, 1)  # release

    @skip_unless_built("05_enc_servo.hex")
    def test_encoder_click_moves_servo_off_minimum(self):
        with Simulator("hc6800_es", hexpath("05_enc_servo.hex")) as sim:
            sim.enable_servo(3, 7, 1000, 2000)
            sim.step_instructions(50_000)
            baseline = sim.servo_pulse_us()
            self.assertAlmostEqual(baseline, 1000, delta=50, msg="count=0 should start near the minimum pulse")

            # One clockwise click: DT=0 held, then a CLK falling edge (P1.6/P1.7 per enc.c).
            sim.set_pin(1, 6, 0)
            sim.step_instructions(5_000)
            sim.set_pin(1, 7, 0)
            sim.step_instructions(5_000)
            sim.set_pin(1, 7, 1)
            sim.set_pin(1, 6, 1)
            sim.step_instructions(50_000)

            self.assertGreater(sim.servo_pulse_us(), baseline + 100,
                                "a clockwise click should move the servo further from the minimum")


class Enc28j60Tests(unittest.TestCase):
    PINS = {"cs": (0, 3), "sck": (0, 2), "mosi": (0, 0), "miso": (0, 1)}

    @skip_unless_built("09_ethernet_diag.hex")
    def test_init_soft_resets_selects_bank0_and_sets_mac_address(self):
        # 09_ethernet_diag.hex is enc28j60Init() plus a BIT_FIELD_CLR and a
        # buffer write+read round trip, without ethernet.c's own UART code
        # (this simulator only models serial_tx() off Timer1 overflow;
        # ethernet.c's uart_init() configures Timer2 instead, a real
        # STC89C52/8052 capability this core doesn't emulate, so its
        # PUTS() calls would block forever here -- see 09_ethernet/
        # diag_no_uart.c's own comment).
        with Simulator("hc6800_es", hexpath("09_ethernet_diag.hex")) as sim:
            sim.enable_enc28j60(**self.PINS)
            sim.step_instructions(2_000_000)

            self.assertEqual(sim.enc28j60_last_opcode(), 0x3A, "should have ended mid a READ_BUF_MEM")
            self.assertEqual(sim.enc28j60_bank(), 0, "enc28j60Init() ends by explicitly selecting bank 0")
            # MAC address (byte-backward: MAADR5=MAC0 .. MAADR0=MAC5), at
            # addresses that -- per this project's own enc28j60.h -- are
            # NOT in name order (MAADR1=0x00, MAADR0=0x01, MAADR3=0x02,
            # MAADR2=0x03, MAADR5=0x04, MAADR4=0x05).
            mac = [sim.enc28j60_register(3, addr) for addr in (0, 1, 2, 3, 4, 5)]
            self.assertEqual(mac, [0x05, 0x06, 0x03, 0x04, 0x01, 0x02])
            # This project's own diag firmware clears ECON1_RXEN (0x04)
            # right after Init() sets it, so ECON1 should be back to 0.
            self.assertEqual(sim.enc28j60_register(0, 0x1F), 0x00)

    @skip_unless_built("09_ethernet_diag.hex")
    def test_buffer_round_trip_advances_pointers_by_exactly_one_per_byte(self):
        # Regression test for an off-by-one found while building this:
        # READ_BUF_MEM's first data byte must be staged *before* it starts
        # shifting out, but committing that "staged" byte again when the
        # *next* byte's output gets prepared double-counted the last byte
        # of every read (buffer_byte_count read 9 for a 4-byte round trip
        # that should read 8 -- 4 written + 4 read).
        with Simulator("hc6800_es", hexpath("09_ethernet_diag.hex")) as sim:
            sim.enable_enc28j60(**self.PINS)
            sim.step_instructions(2_000_000)

            self.assertEqual(sim.enc28j60_buffer_byte_count(), 8)
            self.assertEqual(sim.enc28j60_register(0, 0x00), 4, "ERDPTL should have advanced by exactly 4")
            self.assertEqual(sim.enc28j60_register(0, 0x02), 4, "EWRPTL should have advanced by exactly 4")


class FanTests(unittest.TestCase):
    # 06_fan_tach.hex cycles through duty_table = {30, 50, 70, 100} every
    # 4 (firmware-measured) seconds -- see 06_fan_tach/main.c. At this
    # sim's default 12MHz clock_hz, one real second is clock_hz/12 =
    # 1,000,000 ticks, so instruction counts below are picked to land
    # comfortably inside a given step's ~4-second window (not right on a
    # boundary), using this board's own measured ~1.04 ticks/instruction.
    PINS = {"pwm": (1, 6), "tach": (1, 7)}

    @skip_unless_built("06_fan_tach.hex")
    def test_first_duty_step_and_matching_rpm(self):
        with Simulator("hc6800_es", hexpath("06_fan_tach.hex")) as sim:
            sim.enable_fan(**self.PINS)
            sim.step_instructions(3_000_000)  # well inside [0s, 4s): duty_table[0]=30

            duty = sim.fan_duty_percent()
            self.assertAlmostEqual(duty, 30, delta=5)
            # fan.c's own documented mapping: 0 RPM below
            # FAN_MIN_START_DUTY_PERCENT(20), else linear up to
            # FAN_MAX_RPM(8000) at 100% -- checked against the *measured*
            # duty (not the nominal target) so this only tests the
            # model's internal consistency, not firmware timing precision.
            expected_rpm = 8000 * (duty - 20) // 80
            self.assertAlmostEqual(sim.fan_rpm(), expected_rpm, delta=50)

    @skip_unless_built("06_fan_tach.hex")
    def test_later_duty_step_and_matching_rpm(self):
        with Simulator("hc6800_es", hexpath("06_fan_tach.hex")) as sim:
            sim.enable_fan(**self.PINS)
            sim.step_instructions(13_000_000)  # well inside [12s, 16s): duty_table[3]=100

            duty = sim.fan_duty_percent()
            self.assertGreater(duty, 70, "should be well past the 70% step by now")
            expected_rpm = 8000 * (duty - 20) // 80
            self.assertAlmostEqual(sim.fan_rpm(), expected_rpm, delta=50)

    @skip_unless_built("06_fan_tach.hex")
    def test_rpm_increases_from_first_to_later_step(self):
        with Simulator("hc6800_es", hexpath("06_fan_tach.hex")) as sim:
            sim.enable_fan(**self.PINS)
            sim.step_instructions(3_000_000)
            early_rpm = sim.fan_rpm()
            sim.step_instructions(10_000_000)  # now at ~13M total, inside [12s, 16s)
            late_rpm = sim.fan_rpm()
            self.assertGreater(late_rpm, early_rpm + 1000,
                                "RPM should climb substantially as duty steps up")


if __name__ == "__main__":
    unittest.main()
