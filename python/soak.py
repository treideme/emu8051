"""Long-run soak for 10_net_stack: drive thousands of frames and check the
stack still answers, tracking SP high-water and CPU exceptions.

The unit tests inject a frame or two into a fresh state. The hardware bug
("answers for minutes, then stops replying for good") only appears under
sustained traffic, so this drives:

  - ARP requests for us
  - ICMP echo of varied sizes
  - UDP echo of varied sizes
  - large broadcasts the ENC accepts because BCEN is on for ARP (the laptop
    emits ~342 B mDNS/LLMNR/NetBIOS constantly)
  - optional overflow bursts: inject faster than the MCU can drain

Run:  uv run --no-project --python 3.12 --with numpy python soak.py [rounds]
"""
import os
import sys
import struct

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tests"))

from pysim import Simulator
import test_enc28j60_net as T


class Soak:
    def __init__(self, target="stc89c52rc", rung=3):
        self.sim = Simulator("hc6800_es", T.hexfile(target, rung))
        self.sim.set_xram_size(T.TARGETS[target])
        self.sim.enable_enc28j60(**T.PINS)
        self.seen = 0
        self.sp_max = 0
        self.inject_rc = {}
        # boot: wait for RXEN
        self._run_until(lambda: self.sim.enc28j60_register(0, 0x1F) & 0x04, 3_000_000)

    def _run_until(self, cond, max_ticks):
        used = 0
        while not cond():
            if used >= max_ticks:
                return False
            self.sim.step(1_000)
            self._sample_sp()
            used += 1_000
        return True

    def _sample_sp(self):
        sp = self.sim.peek_sfr(0x81)
        if sp > self.sp_max:
            self.sp_max = sp

    def step(self, ticks):
        done = 0
        while done < ticks:
            self.sim.step(1_000)
            self._sample_sp()
            done += 1_000

    def inject(self, frame):
        rc = self.sim.enc28j60_inject(frame)
        self.inject_rc[rc] = self.inject_rc.get(rc, 0) + 1
        return rc

    def new_tx(self):
        f = self.sim.enc28j60_tx_frames(self.seen)
        self.seen += len(f)
        return f

    def enc_state(self):
        return dict(
            EIR=self.sim.enc28j60_register(0, 0x1C),
            ESTAT=self.sim.enc28j60_register(0, 0x1D),
            ECON1=self.sim.enc28j60_register(0, 0x1F),
            PKTCNT=self.sim.enc28j60_register(1, 0x19),
            ERXRDPT=self.sim.enc28j60_register(0, 0x0C)
            | (self.sim.enc28j60_register(0, 0x0D) << 8),
            ERXWRPT=self.sim.enc28j60_register(0, 0x0E)
            | (self.sim.enc28j60_register(0, 0x0F) << 8),
        )


def ping_frame(payload, seq):
    return T.eth(T.OUR_MAC, T.PEER_MAC, 0x0800,
                 T.ipv4(T.PEER_IP, T.OUR_IP, 1, T.icmp_echo(0x77, seq, payload)))


def udp_frame(payload):
    return T.eth(T.OUR_MAC, T.PEER_MAC, 0x0800,
                 T.ipv4(T.PEER_IP, T.OUR_IP, 17,
                        T.udp(T.PEER_IP, T.OUR_IP, 4000, 7, payload)))


def arp_frame():
    return T.eth(T.BCAST_MAC, T.PEER_MAC, 0x0806,
                 T.arp(1, T.PEER_MAC, T.PEER_IP, b"\0" * 6, T.OUR_IP))


def broadcast_frame(n=342):
    """A big broadcast the ENC accepts (BCEN) but the stack must ignore:
    UDP to port 5353 (mDNS) addressed to a multicast IP, not ours."""
    body = T.udp(T.PEER_IP, bytes([224, 0, 0, 251]), 5353, 5353, b"\xAA" * (n - 42))
    return T.eth(T.BCAST_MAC, T.PEER_MAC, 0x0800,
                 T.ipv4(T.PEER_IP, bytes([224, 0, 0, 251]), 17, body))


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    burst = "--burst" in sys.argv
    s = Soak()
    print(f"boot ok, ECON1={s.sim.enc28j60_register(0,0x1F):02X}")

    answered = 0
    missed = []
    for i in range(rounds):
        kind = i % 4
        if kind == 0:
            frame, expect = arp_frame(), 1
        elif kind == 1:
            frame, expect = ping_frame(bytes(range(32)), i & 0xFFFF), 1
        elif kind == 2:
            frame, expect = udp_frame(b"soak%04d" % (i & 0xFFFF)), 1
        else:
            frame, expect = broadcast_frame(), 0

        if burst and kind == 3:
            # flood: inject several big broadcasts back to back without
            # giving the MCU time to drain, to force an RX overflow
            for _ in range(8):
                s.inject(broadcast_frame())

        rc = s.inject(frame)
        before = s.seen
        # generous: a 100 B echo takes ~100 ms of simulated SPI
        s._run_until(lambda: s.sim.enc28j60_tx_count() >= before + expect, 4_000_000)
        got = len(s.new_tx())
        if got >= expect:
            answered += 1
        else:
            missed.append((i, kind, rc, s.enc_state()))
            if len(missed) > 6:
                break

    print(f"answered {answered}/{rounds}")
    print("inject rc counts:", s.inject_rc)
    print("SP high-water: 0x%02X (%d)" % (s.sp_max, s.sp_max))
    print("exceptions:", s.sim.exceptions())
    print("final ENC:", s.enc_state())
    for m in missed[:6]:
        print("  MISS round=%d kind=%d rc=%d %s" % m)
    s.sim.close()
    return 0 if not missed else 1


if __name__ == "__main__":
    sys.exit(main())
