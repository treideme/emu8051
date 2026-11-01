"""The address range each IAP profile accepts, from the host side.

The bootloader tests in test_iap_bootloader.py drive the IAP SFRs from real
firmware; these only pin down where each profile's flash starts and ends,
which is the one thing that differs between the two STC89-style parts.

    uv run --no-project python -m unittest discover -s tests -v   (from python/)
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from pysim import Simulator  # noqa: E402


class IapProfileRangeTests(unittest.TestCase):
    def sim(self, profile):
        s = Simulator("hc6800_es")
        self.addCleanup(s.close)
        s.enable_iap(profile)
        return s

    def assert_range(self, s, lo, hi):
        self.assertEqual(s.iap_peek(lo), 0xFF)          # erased
        self.assertEqual(s.iap_peek(hi - 1), 0xFF)
        self.assertEqual(s.iap_peek(lo - 1), -1)
        self.assertEqual(s.iap_peek(hi), -1)
        s.iap_poke(lo, 0x5A)
        s.iap_poke(hi - 1, 0xA5)
        self.assertEqual((s.iap_peek(lo), s.iap_peek(hi - 1)), (0x5A, 0xA5))
        for outside in (lo - 1, hi):
            with self.assertRaises(ValueError):
                s.iap_poke(outside, 0)

    def test_stc89c52rc_data_flash_is_2000h_to_2fffh(self):
        self.assert_range(self.sim(Simulator.IAP_STC89C52RC), 0x2000, 0x3000)

    def test_stc12c2052ad_data_flash_is_0800h_to_17ffh(self):
        self.assert_range(self.sim(Simulator.IAP_STC12C2052AD), 0x0800, 0x1800)

    def test_an_unknown_profile_falls_back_to_the_stc89(self):
        self.assert_range(self.sim(7), 0x2000, 0x3000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
