"""Network-stack tests for stc89c52-staging's 10_net_stack firmware.

Each rung of the experiment (ARP; IPv4+ICMP echo; UDP echo; DHCP client) is a
separate build, net_stc89c52rc_rung<N>.hex, so a test runs against exactly the
code that rung adds. Frames are built here with independently computed
checksums, delivered into the ENC28J60 model's RX ring (which applies the
chip's receive filter and ring layout), and every frame the firmware
transmits is checked field by field, checksums included.

Run from python/:  EMU8051_DEMO_BUILD=<staging>/build-direct pytest tests -q

Fidelity: the CPU is cycle-counted, so the simulated latencies printed by
the timing tests are real 12 MHz machine-cycle counts for this code. The
ENC28J60 is modelled at the SPI-bit level for protocol, not electrically:
nothing here says whether the real part accepts a ~30 kHz bit-banged SCK.
"""

import os
import struct
import unittest

from pysim import Simulator

BUILD = os.environ.get(
    "EMU8051_DEMO_BUILD",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "demo-projects", "build"),
)

OUR_MAC = bytes.fromhex("020000000001")
OUR_IP = bytes([192, 168, 7, 2])        # NET_STATIC_IP, rungs 1-3
PEER_MAC = bytes.fromhex("a82bdd93cfef")
PEER_IP = bytes([192, 168, 7, 1])
BCAST_MAC = b"\xff" * 6
XID = bytes.fromhex("5ad10001")         # net.c
MTU = 576
PINS = dict(cs=(0, 3), sck=(0, 2), mosi=(0, 0), miso=(0, 1))


def hexfile(rung):
    return os.path.join(BUILD, f"net_stc89c52rc_rung{rung}.hex")


def needs(rung):
    return unittest.skipUnless(os.path.isfile(hexfile(rung)),
                               f"build 10_net_stack first (missing {hexfile(rung)})")


# --- frame construction ------------------------------------------------------

def csum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def eth(dst, src, etype, payload):
    return dst + src + struct.pack("!H", etype) + payload


def arp(op, sha, spa, tha, tpa):
    return struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + sha + spa + tha + tpa


def ipv4(src, dst, proto, payload, ident=0x1234, flags_frag=0, ttl=64, bad_sum=False):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, flags_frag,
                      ttl, proto, 0, src, dst)
    s = csum(hdr) ^ (0x5555 if bad_sum else 0)
    return hdr[:10] + struct.pack("!H", s) + hdr[12:] + payload


def icmp_echo(ident, seq, data, type_=8):
    body = struct.pack("!BBHHH", type_, 0, 0, ident, seq) + data
    return body[:2] + struct.pack("!H", csum(body)) + body[4:]


def udp(src_ip, dst_ip, sport, dport, data, with_sum=True):
    length = 8 + len(data)
    hdr = struct.pack("!HHHH", sport, dport, length, 0)
    s = 0
    if with_sum:
        pseudo = src_ip + dst_ip + struct.pack("!BBH", 0, 17, length)
        s = csum(pseudo + hdr + data) or 0xFFFF
    return hdr[:6] + struct.pack("!H", s) + data


def bootp_reply(msg_type, yiaddr, server, xid=XID, chaddr=OUR_MAC, extra_opts=b""):
    fixed = struct.pack("!BBBB4sHH4s4s4s4s", 2, 1, 6, 0, xid, 0, 0x8000,
                        b"\0" * 4, yiaddr, server, b"\0" * 4)
    fixed += chaddr + b"\0" * 10 + b"\0" * 64 + b"\0" * 128
    opts = (bytes([99, 130, 83, 99]) + extra_opts + bytes([53, 1, msg_type]) +
            bytes([54, 4]) + server + bytes([1, 4, 255, 255, 255, 0]) +
            bytes([3, 4]) + server + bytes([51, 4, 0, 0, 0x0E, 0x10]) + b"\xff")
    return fixed + opts


def dhcp_frame(msg_type, yiaddr, server=PEER_IP, **kw):
    bootp = bootp_reply(msg_type, yiaddr, server, **kw)
    u = udp(server, b"\xff" * 4, 67, 68, bootp)
    return eth(BCAST_MAC, PEER_MAC, 0x0800, ipv4(server, b"\xff" * 4, 17, u))


# --- frame checking ------------------------------------------------------------

class Parsed:
    def __init__(self, f: bytes):
        self.raw = f
        self.dst, self.src = f[0:6], f[6:12]
        self.etype = struct.unpack("!H", f[12:14])[0]
        self.body = f[14:]
        if self.etype == 0x0800:
            ihl = (f[14] & 0x0F) * 4
            self.ip = f[14:14 + ihl]
            total = struct.unpack("!H", f[16:18])[0]
            self.ip_total = total
            self.proto = f[23]
            self.ip_src, self.ip_dst = f[26:30], f[30:34]
            self.ttl = f[22]
            self.l4 = f[14 + ihl:14 + total]


def assert_ip_ok(tc, p: Parsed):
    tc.assertEqual(p.etype, 0x0800)
    tc.assertEqual(p.ip[0], 0x45)
    tc.assertEqual(csum(p.ip), 0, "IP header checksum invalid")
    tc.assertLessEqual(14 + p.ip_total, len(p.raw))


# --- harness ---------------------------------------------------------------------

class Net:
    CHUNK = 20_000

    def __init__(self, rung):
        self.sim = Simulator("hc6800_es", hexfile(rung))
        self.sim.enable_enc28j60(**PINS)
        self.seen = 0
        # boot: wait for enc_init() to enable reception (ECON1.RXEN)
        self.run_until(lambda: self.sim.enc28j60_register(0, 0x1F) & 0x04, 3_000_000)
        self.boot_ticks = self.sim.tick_count

    def close(self):
        self.sim.close()

    def run_until(self, cond, max_ticks):
        used = 0
        while not cond():
            if used >= max_ticks:
                raise AssertionError(f"condition not met within {max_ticks} ticks")
            self.sim.step(self.CHUNK)
            used += self.CHUNK
        return used

    def inject(self, frame):
        rc = self.sim.enc28j60_inject(frame)
        return rc

    def new_tx(self):
        frames = self.sim.enc28j60_tx_frames(self.seen)
        self.seen += len(frames)
        return frames

    def expect_tx(self, n=1, max_ticks=2_000_000):
        start = self.sim.tick_count
        target = self.seen + n
        self.run_until(lambda: self.sim.enc28j60_tx_count() >= target, max_ticks)
        self.last_latency_ticks = self.sim.tick_count - start
        return [Parsed(f) for f in self.new_tx()]

    def expect_silence(self, ticks=600_000):
        self.sim.step(ticks)
        return self.new_tx()

    def settle(self):
        """Let the firmware drain everything it has queued."""
        self.run_until(lambda: self.sim.enc28j60_register(1, 0x19) == 0, 3_000_000)


class _Base(unittest.TestCase):
    RUNG = None

    def setUp(self):
        self.net = Net(self.RUNG)

    def tearDown(self):
        # Illegal opcodes, stack overflow into the SFRs, etc. are recorded by
        # the core rather than raised; a test that passed on its frames must
        # not have hit one along the way.
        exc = self.net.sim.exceptions()
        self.net.close()
        self.assertEqual(exc, [], f"CPU exceptions during test: {exc}")

    # shared checks
    def ping(self, payload, ident=0x77, seq=1, dst_ip=OUR_IP):
        req = eth(OUR_MAC, PEER_MAC, 0x0800,
                  ipv4(PEER_IP, dst_ip, 1, icmp_echo(ident, seq, payload)))
        self.assertEqual(self.net.inject(req), Simulator.ENC_RX_OK)
        return req

    def check_echo_reply(self, p: Parsed, payload, ident=0x77, seq=1, src_ip=OUR_IP):
        assert_ip_ok(self, p)
        self.assertEqual(p.dst, PEER_MAC)
        self.assertEqual(p.src, OUR_MAC)
        self.assertEqual(p.ip_src, src_ip)
        self.assertEqual(p.ip_dst, PEER_IP)
        self.assertEqual(p.proto, 1)
        self.assertEqual(p.ttl, 64)
        self.assertEqual(p.l4[0], 0, "ICMP type must be echo reply")
        self.assertEqual(csum(p.l4), 0, "ICMP checksum invalid")
        self.assertEqual(struct.unpack("!HH", p.l4[4:8]), (ident, seq))
        self.assertEqual(p.l4[8:], payload)

    def arp_request(self, tpa):
        f = eth(BCAST_MAC, PEER_MAC, 0x0806, arp(1, PEER_MAC, PEER_IP, b"\0" * 6, tpa))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def check_arp_reply(self, p: Parsed, ip=OUR_IP):
        self.assertEqual(p.etype, 0x0806)
        self.assertEqual(len(p.raw), 60, "padded to the Ethernet minimum")
        self.assertEqual(p.dst, PEER_MAC)
        self.assertEqual(p.src, OUR_MAC)
        a = p.body
        self.assertEqual(struct.unpack("!HHBBH", a[:8]), (1, 0x0800, 6, 4, 2))
        self.assertEqual(a[8:14], OUR_MAC)
        self.assertEqual(a[14:18], ip)
        self.assertEqual(a[18:24], PEER_MAC)
        self.assertEqual(a[24:28], PEER_IP)


# --- rung 1: ARP ---------------------------------------------------------------------

@needs(1)
class Rung1Arp(_Base):
    RUNG = 1

    def test_boot_enables_reception_quickly(self):
        # enc_init()'s errata wait is ~0.5 s of busy loop at 12 MHz
        self.assertLess(self.net.boot_ticks, 1_500_000)

    def test_arp_request_for_our_ip_is_answered(self):
        self.arp_request(OUR_IP)
        (p,) = self.net.expect_tx()
        self.check_arp_reply(p)

    def test_arp_for_other_ip_is_ignored(self):
        self.arp_request(bytes([192, 168, 7, 99]))
        self.assertEqual(self.net.expect_silence(), [])

    def test_arp_reply_is_not_answered(self):
        f = eth(OUR_MAC, PEER_MAC, 0x0806, arp(2, PEER_MAC, PEER_IP, OUR_MAC, OUR_IP))
        self.net.inject(f)
        self.assertEqual(self.net.expect_silence(), [])

    def test_receive_filter_is_unicast_plus_broadcast(self):
        other = eth(bytes.fromhex("020000000099"), PEER_MAC, 0x0806,
                    arp(1, PEER_MAC, PEER_IP, b"\0" * 6, OUR_IP))
        self.assertEqual(self.net.inject(other), Simulator.ENC_RX_FILTERED)
        mcast = eth(bytes.fromhex("01005e000001"), PEER_MAC, 0x0806,
                    arp(1, PEER_MAC, PEER_IP, b"\0" * 6, OUR_IP))
        self.assertEqual(self.net.inject(mcast), Simulator.ENC_RX_FILTERED)

    def test_many_arp_requests_across_ring_wrap(self):
        for i in range(60):          # ~60 * 70 B ring bytes: wraps the 3 KB ring
            self.arp_request(OUR_IP)
            (p,) = self.net.expect_tx()
            self.check_arp_reply(p)


# --- rung 2: IPv4 + ICMP ---------------------------------------------------------------

@needs(2)
class Rung2Icmp(_Base):
    RUNG = 2

    def test_ping_32_bytes(self):
        data = bytes(range(32))
        self.ping(data)
        (p,) = self.net.expect_tx()
        self.check_echo_reply(p, data)
        print(f"\n  [sim] 32 B ping latency: {self.net.last_latency_ticks / 1e3:.1f} ms")

    def test_ping_at_mtu(self):
        data = bytes((i * 7) & 0xFF for i in range(MTU - 28))   # 576 B datagram
        self.ping(data)
        (p,) = self.net.expect_tx(max_ticks=5_000_000)
        self.check_echo_reply(p, data)
        print(f"\n  [sim] {MTU} B ping latency: {self.net.last_latency_ticks / 1e3:.1f} ms")

    def test_ping_above_mtu_is_dropped(self):
        self.ping(bytes(MTU - 28 + 2))
        self.assertEqual(self.net.expect_silence(2_000_000), [])

    def test_ping_to_other_ip_is_dropped(self):
        self.ping(b"x" * 8, dst_ip=bytes([192, 168, 7, 99]))
        self.assertEqual(self.net.expect_silence(), [])

    def test_bad_ip_header_checksum_is_dropped(self):
        f = eth(OUR_MAC, PEER_MAC, 0x0800,
                ipv4(PEER_IP, OUR_IP, 1, icmp_echo(1, 1, b"abc"), bad_sum=True))
        self.net.inject(f)
        self.assertEqual(self.net.expect_silence(), [])

    def test_fragment_is_dropped(self):
        f = eth(OUR_MAC, PEER_MAC, 0x0800,
                ipv4(PEER_IP, OUR_IP, 1, icmp_echo(1, 1, b"abcd"), flags_frag=0x2000))
        self.net.inject(f)
        self.assertEqual(self.net.expect_silence(), [])

    def test_odd_length_payload(self):
        data = b"odd-length!"
        self.ping(data)
        (p,) = self.net.expect_tx()
        self.check_echo_reply(p, data)

    def test_back_to_back_pings_across_ring_wrap(self):
        # two queued at once, then a run long enough to wrap the 3 KB RX ring
        a, b = bytes(range(100)), bytes(range(100, 200))
        self.ping(a, seq=1)
        self.ping(b, seq=2)
        p1, p2 = self.net.expect_tx(2, max_ticks=4_000_000)
        self.check_echo_reply(p1, a, seq=1)
        self.check_echo_reply(p2, b, seq=2)
        for seq in range(3, 15):
            data = bytes((seq + i) & 0xFF for i in range(400))
            self.ping(data, seq=seq)
            (p,) = self.net.expect_tx(max_ticks=4_000_000)
            self.check_echo_reply(p, data, seq=seq)

    def test_arp_still_answered(self):
        self.arp_request(OUR_IP)
        (p,) = self.net.expect_tx()
        self.check_arp_reply(p)


# --- rung 3: UDP ---------------------------------------------------------------------------

@needs(3)
class Rung3Udp(_Base):
    RUNG = 3

    def udp_to_us(self, dport, data, with_sum=True):
        u = udp(PEER_IP, OUR_IP, 40000, dport, data, with_sum)
        f = eth(OUR_MAC, PEER_MAC, 0x0800, ipv4(PEER_IP, OUR_IP, 17, u))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def check_udp_echo(self, p, data, expect_sum):
        assert_ip_ok(self, p)
        self.assertEqual((p.dst, p.src), (PEER_MAC, OUR_MAC))
        self.assertEqual((p.ip_src, p.ip_dst, p.proto), (OUR_IP, PEER_IP, 17))
        sport, dport, length, s = struct.unpack("!HHHH", p.l4[:8])
        self.assertEqual((sport, dport, length), (7, 40000, 8 + len(data)))
        self.assertEqual(p.l4[8:], data)
        if expect_sum:
            pseudo = p.ip_src + p.ip_dst + struct.pack("!BBH", 0, 17, length)
            self.assertEqual(csum(pseudo + p.l4), 0, "UDP checksum invalid")
        else:
            self.assertEqual(s, 0)

    def test_udp_echo_with_checksum(self):
        data = b"hello from the host"
        self.udp_to_us(7, data)
        (p,) = self.net.expect_tx()
        self.check_udp_echo(p, data, True)

    def test_udp_echo_without_checksum(self):
        data = bytes(range(64))
        self.udp_to_us(7, data, with_sum=False)
        (p,) = self.net.expect_tx()
        self.check_udp_echo(p, data, False)

    def test_udp_echo_at_mtu(self):
        data = bytes((i * 3) & 0xFF for i in range(MTU - 28))
        self.udp_to_us(7, data)
        (p,) = self.net.expect_tx(max_ticks=5_000_000)
        self.check_udp_echo(p, data, True)

    def test_udp_other_port_is_ignored(self):
        self.udp_to_us(9, b"discard me")
        self.assertEqual(self.net.expect_silence(), [])

    def test_ping_still_answered(self):
        self.ping(b"12345678")
        (p,) = self.net.expect_tx()
        self.check_echo_reply(p, b"12345678")


# --- rung 4: DHCP ---------------------------------------------------------------------------

@needs(4)
class Rung4Dhcp(_Base):
    RUNG = 4
    LEASE = bytes([192, 168, 7, 50])

    def check_dhcp_client_frame(self, p, msg_type):
        assert_ip_ok(self, p)
        self.assertEqual((p.dst, p.src), (BCAST_MAC, OUR_MAC))
        self.assertEqual((p.ip_src, p.ip_dst, p.proto), (b"\0" * 4, b"\xff" * 4, 17))
        sport, dport, length, s = struct.unpack("!HHHH", p.l4[:8])
        self.assertEqual((sport, dport), (68, 67))
        bootp = p.l4[8:]
        self.assertEqual(length, 8 + len(bootp))
        self.assertGreaterEqual(len(bootp), 300)
        self.assertEqual(bootp[0:4], bytes([1, 1, 6, 0]))
        self.assertEqual(bootp[4:8], XID)
        self.assertEqual(bootp[28:34], OUR_MAC)
        self.assertEqual(bootp[236:240], bytes([99, 130, 83, 99]))
        opts = self.options(bootp[240:])
        self.assertEqual(opts.get(53), bytes([msg_type]))
        return opts

    @staticmethod
    def options(raw):
        out, i = {}, 0
        while i < len(raw) and raw[i] != 255:
            if raw[i] == 0:
                i += 1
                continue
            out[raw[i]] = raw[i + 2:i + 2 + raw[i + 1]]
            i += 2 + raw[i + 1]
        return out

    def discover(self):
        (p,) = self.net.expect_tx()
        self.check_dhcp_client_frame(p, 1)
        return p

    def test_discover_at_boot(self):
        self.discover()

    def test_full_exchange_then_arp_and_ping_on_leased_address(self):
        self.discover()
        self.assertEqual(self.net.inject(dhcp_frame(2, self.LEASE)), Simulator.ENC_RX_OK)
        (req,) = self.net.expect_tx()
        opts = self.check_dhcp_client_frame(req, 3)
        self.assertEqual(opts.get(50), self.LEASE)
        self.assertEqual(opts.get(54), PEER_IP)
        self.net.inject(dhcp_frame(5, self.LEASE))
        self.net.settle()
        self.arp_request(self.LEASE)
        (a,) = self.net.expect_tx()
        self.check_arp_reply(a, ip=self.LEASE)
        self.ping(b"leased", dst_ip=self.LEASE)
        (e,) = self.net.expect_tx()
        self.check_echo_reply(e, b"leased", src_ip=self.LEASE)

    def test_not_answering_arp_before_bound(self):
        self.discover()
        self.arp_request(self.LEASE)
        self.arp_request(bytes([0, 0, 0, 0]))
        self.assertEqual(self.net.expect_silence(), [])

    def test_offer_with_wrong_xid_is_ignored(self):
        self.discover()
        self.net.inject(dhcp_frame(2, self.LEASE, xid=b"\x01\x02\x03\x04"))
        self.assertEqual(self.net.expect_silence(), [])

    def test_offer_for_other_client_is_ignored(self):
        self.discover()
        self.net.inject(dhcp_frame(2, self.LEASE, chaddr=bytes.fromhex("020000000042")))
        self.assertEqual(self.net.expect_silence(), [])

    def test_long_option_before_message_type_is_skipped(self):
        self.discover()
        long_opt = bytes([119, 40]) + bytes(range(40))      # domain search, > 32 B
        self.net.inject(dhcp_frame(2, self.LEASE, extra_opts=long_opt))
        (req,) = self.net.expect_tx()
        self.check_dhcp_client_frame(req, 3)

    def test_nak_restarts_with_discover(self):
        self.discover()
        self.net.inject(dhcp_frame(2, self.LEASE))
        self.check_dhcp_client_frame(self.net.expect_tx()[0], 3)
        self.net.inject(dhcp_frame(6, self.LEASE))
        (again,) = self.net.expect_tx()
        self.check_dhcp_client_frame(again, 1)

    def test_discover_is_retransmitted(self):
        first = self.net.sim.tick_count
        self.discover()
        (p,) = self.net.expect_tx(max_ticks=30_000_000)
        self.check_dhcp_client_frame(p, 1)
        print(f"\n  [sim] DISCOVER retransmit interval: "
              f"{(self.net.sim.tick_count - first) / 1e6:.2f} s")


if __name__ == "__main__":
    unittest.main()
