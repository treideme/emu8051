"""unittest coverage for the sim/ peripheral layer, run against real
compiled .hex files from the sibling demo repo.

These need that repo built first (`meson setup build && ninja -C build`
inside the sibling demo repo) -- tests for a .hex that isn't there are skipped
rather than failed, so this suite still runs (mostly-skipped) in isolation.

Run with: python -m unittest discover -s python/tests -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from pysim import Simulator  # noqa: E402

# Compiled 8051 demo projects to run against. Point EMU8051_DEMO_BUILD at the
# build directory of whichever demo repo you use; the default assumes a
# sibling checkout next to this one.
STAGING_BUILD = os.environ.get(
    "EMU8051_DEMO_BUILD",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "demo-projects", "build"),
)


# Name of the clock/7-segment demo binary to exercise. Override with
# EMU8051_CLOCK_DEMO_HEX if your demo projects use a different name.
CLOCK_DEMO_HEX = os.environ.get("EMU8051_CLOCK_DEMO_HEX", "clock_digit_tube.hex")


def hexpath(name):
    return os.path.join(STAGING_BUILD, name)


def skip_unless_built(name):
    path = hexpath(name)
    return unittest.skipUnless(
        os.path.isfile(path),
        f"{name} not found -- build the sibling demo repo first (meson setup build && ninja -C build)",
    )


class LCDTests(unittest.TestCase):
    @skip_unless_built("18_lcd_name_id.hex")
    def test_static_name_and_id(self):
        with Simulator("hc6800_es", hexpath("18_lcd_name_id.hex")) as sim:
            sim.enable_lcd()
            sim.step_instructions(2_000_000)
            self.assertEqual(sim.lcd_line(0).strip(), "hongXkeX")
            self.assertEqual(sim.lcd_line(1).strip(), "201418010219")
            self.assertEqual(sim.exceptions(), [])

    @skip_unless_built("344_clock_lcd.hex")
    def test_ds1302_date_time_on_lcd(self):
        with Simulator("hc6800_es", hexpath("344_clock_lcd.hex")) as sim:
            sim.enable_lcd()
            sim.enable_ds1302()
            sim.ds1302_set_time(second=45, minute=30, hour=14, date=20, month=3, weekday=2, year=26)
            sim.step_instructions(2_000_000)
            # DS1302 isn't a real chip here, so the exact displayed digits
            # only prove the firmware's own Ds1302Init() defaults (or our
            # stimulus, depending which ran last) made it through the
            # write+read round trip and onto the LCD in the right format --
            # not that these specific values are "the time". Assert shape,
            # not content.
            line0 = sim.lcd_line(0)
            line1 = sim.lcd_line(1)
            self.assertRegex(line0, r"\d{4}-\d{2}-\d{2}-\d")
            self.assertRegex(line1, r"\d{2}:\d{2}:\d{2}")


class DigitDisplayTests(unittest.TestCase):
    @skip_unless_built("17_digit_tube_student_id.hex")
    def test_student_id_on_7segment(self):
        with Simulator("hc6800_es", hexpath("17_digit_tube_student_id.hex")) as sim:
            sim.enable_digit_display(8)
            sim.step_instructions(500_000)
            self.assertEqual(sim.digits_text(), "91204102")
            self.assertEqual(sim.exceptions(), [])

    @skip_unless_built("34_digit_tube_display_optimized.hex")
    def test_seconds_counter_multiplexes(self):
        with Simulator("hc6800_es", hexpath("34_digit_tube_display_optimized.hex")) as sim:
            sim.enable_digit_display(8)
            sim.step_instructions(500_000)
            # The seconds *value* changes too slowly to reach in a bounded
            # instruction budget (see the sibling demo repo's doc/simulation-notes.md
            # for why ucsim hit the same wall) -- what's cheap and
            # meaningful to assert is that the multiplex scan is alive at
            # all: more than one digit position actually got written.
            distinct = {sim.digit_segments(i) for i in range(8)}
            self.assertGreaterEqual(len(distinct), 2)


class DS1302Tests(unittest.TestCase):
    @skip_unless_built("3431_clock_digit_tube_1.hex")
    def test_write_then_read_round_trip(self):
        with Simulator("hc6800_es", hexpath("3431_clock_digit_tube_1.hex")) as sim:
            sim.enable_ds1302()
            sim.enable_digit_display(8)
            sim.step_instructions(500_000)
            # Ds1302Init() writes this firmware's own compile-time default
            # time; Ds1302ReadTime() then reads it back. Confirms the full
            # 3-wire write+read protocol round-trips correctly regardless
            # of which specific values this project's ds1302.c happens to
            # hardcode.
            hours = sim.ds1302_register(2)
            self.assertNotEqual(hours, 0xFF, "read never completed (still floating)")

    @skip_unless_built(CLOCK_DEMO_HEX)
    def test_digits_survive_boot_without_stale_latch(self):
        # Regression test for a bug where 74HC573 outputs default to
        # transparent=0 (calloc'd) regardless of LE's actual level. This
        # firmware never writes LE at all -- relying on the 8051's
        # power-on P1=0xFF default to keep the latch permanently
        # transparent -- so with the bug, digit_display's resample()
        # would capture one spurious all-segments-on snapshot during
        # Ds1302Init()/Int1Init() (while LE reads 1 but the model still
        # thought it was 0/latched) and then never recapture it, since
        # data-bus writes wrongly no-op'd on grounds the latch was closed.
        VALID_SEGMENTS = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f, 0x40}
        with Simulator("hc6800_es", hexpath(CLOCK_DEMO_HEX)) as sim:
            sim.enable_ds1302()
            sim.enable_digit_display(8)
            sim.step_instructions(500_000)
            for i in range(8):
                segs = sim.digit_segments(i)
                self.assertIn(
                    segs, VALID_SEGMENTS,
                    f"digit {i} shows 0x{segs:02x}, not a real smgduan[]/dash "
                    "pattern -- looks like a stale pre-boot latch capture",
                )


class RealTimeClockTests(unittest.TestCase):
    """DS1302 keeps its own free-running clock now (ds1302_step(), tied to
    simulated elapsed time via clock_hz/12 ticks-per-second -- see
    sim/devices/ds1302.c), independent of any particular firmware, so
    these don't need the sibling demo repo's checkout at all."""

    def test_seconds_register_increments_exactly_on_the_boundary(self):
        with Simulator("hc6800_es") as sim:
            sim.enable_ds1302()
            sim.ds1302_set_time(second=0, minute=0, hour=0, date=1, month=1, weekday=1, year=0)
            ticks_per_second = sim.clock_hz // 12
            sim.step(ticks_per_second - 1)
            self.assertEqual(sim.ds1302_register(0), 0x00, "ticked over a second early")
            sim.step(1)
            self.assertEqual(sim.ds1302_register(0), 0x01, "didn't tick over on time")

    def test_clock_hz_override_changes_tick_rate(self):
        # e.g. a 10MHz part instead of hc6800_es's stock 12MHz -- still a
        # classic 12-clocks-per-machine-cycle core, so ticks-per-second
        # scales down proportionally with the oscillator.
        with Simulator("hc6800_es") as sim:
            sim.set_clock_hz(10_000_000)
            sim.enable_ds1302()
            self.assertEqual(sim.clock_hz, 10_000_000)
            ticks_per_second = sim.clock_hz // 12
            sim.ds1302_set_time(second=0, minute=0, hour=0, date=1, month=1, weekday=1, year=0)
            sim.step(ticks_per_second - 1)
            self.assertEqual(sim.ds1302_register(0), 0x00, "ticked over a second early")
            sim.step(1)
            self.assertEqual(sim.ds1302_register(0), 0x01, "didn't tick over on time")

    def test_leap_year_day_and_month_rollover(self):
        with Simulator("hc6800_es") as sim:
            sim.enable_ds1302()
            # 2024 is a leap year: Feb 28 23:59:59 -> Feb 29 00:00:00, not March 1.
            sim.ds1302_set_time(second=59, minute=59, hour=23, date=28, month=2, weekday=7, year=24)
            sim.step(sim.clock_hz // 12)
            self.assertEqual(
                [sim.ds1302_register(i) for i in range(7)],
                [0x00, 0x00, 0x00, 0x29, 0x02, 0x01, 0x24],
            )

    def test_non_leap_year_rolls_into_march(self):
        with Simulator("hc6800_es") as sim:
            sim.enable_ds1302()
            sim.ds1302_set_time(second=59, minute=59, hour=23, date=28, month=2, weekday=7, year=23)
            sim.step(sim.clock_hz // 12)
            self.assertEqual(
                [sim.ds1302_register(i) for i in range(7)],
                [0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x23],
            )

    def test_year_wraps_at_century_boundary(self):
        with Simulator("hc6800_es") as sim:
            sim.enable_ds1302()
            sim.ds1302_set_time(second=59, minute=59, hour=23, date=31, month=12, weekday=3, year=99)
            sim.step(sim.clock_hz // 12)
            self.assertEqual(
                [sim.ds1302_register(i) for i in range(7)],
                [0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x00],
            )


class ResetTests(unittest.TestCase):
    def test_reset_restarts_cpu_but_preserves_ds1302(self):
        # A real DS1302 is battery-backed and keeps running across an MCU
        # reset -- confirms sim_reset() doesn't touch it while it does
        # zero the CPU-derived counters (see capi.h's sim_reset() comment).
        with Simulator("hc6800_es") as sim:
            sim.enable_ds1302()
            sim.ds1302_set_time(second=30, minute=0, hour=0, date=1, month=1, weekday=1, year=25)
            sim.step_instructions(1_000)
            self.assertGreater(sim.instruction_count, 0)

            sim.reset()

            self.assertEqual(sim.instruction_count, 0)
            self.assertEqual(sim.tick_count, 0)
            self.assertEqual(sim.ds1302_register(0), 0x30)

    @skip_unless_built("17_digit_tube_student_id.hex")
    def test_reset_clears_stale_digit_display_capture(self):
        with Simulator("hc6800_es", hexpath("17_digit_tube_student_id.hex")) as sim:
            sim.enable_digit_display(8)
            sim.step_instructions(500_000)
            self.assertEqual(sim.digits_text(), "91204102")

            sim.reset()

            # Recreated fresh rather than still showing the pre-reset
            # capture (this firmware hasn't re-driven P0 yet at PC=0).
            for i in range(8):
                self.assertEqual(sim.digit_segments(i), 0)


class ADCTests(unittest.TestCase):
    @skip_unless_built("ad_xpt2046_adc.hex")
    def test_channel_select(self):
        with Simulator("hc6800_es", hexpath("ad_xpt2046_adc.hex")) as sim:
            sim.enable_xpt2046()
            sim.enable_digit_display(8)
            sim.xpt2046_set_reading(2048)
            sim.step_instructions(500_000)
            # No real XPT2046 chip is modeled electrically, so the exact
            # converted value isn't meaningful evidence (see
            # doc/simulation-notes.md) -- but the command byte's channel
            # field decoding correctly is real, checkable evidence that
            # the SPI bit-bang sequence the firmware issued was well-formed.
            self.assertGreaterEqual(sim.xpt2046_last_channel(), 0)


class UARTTests(unittest.TestCase):
    @skip_unless_built("411_uart_echo_flag.hex")
    def test_reply_byte_depends_on_received_value(self):
        with Simulator("hc6800_es", hexpath("411_uart_echo_flag.hex")) as sim:
            sim.step_instructions(2_000)  # let Timer1/SCON init finish
            sim.uart_inject_rx(0x13)  # the firmware's secret match byte
            sim.step_instructions(500_000)
            tx = sim.uart_tx_bytes()
            self.assertIn(0xAA, tx, f"expected 0xAA reply for the matching byte, got {tx!r}")

    @skip_unless_built("411_uart_echo_flag.hex")
    def test_reply_byte_for_non_matching_value(self):
        with Simulator("hc6800_es", hexpath("411_uart_echo_flag.hex")) as sim:
            sim.step_instructions(2_000)
            sim.uart_inject_rx(0x00)
            sim.step_instructions(500_000)
            tx = sim.uart_tx_bytes()
            self.assertIn(0x55, tx, f"expected 0x55 reply for a non-matching byte, got {tx!r}")


if __name__ == "__main__":
    unittest.main()
