"""Network-stack tests for stc89c52-staging's 10_net_stack firmware.

Each rung of the experiment (1 ARP; 2 IPv4+ICMP echo; 3 UDP echo; 4 DHCP
client; 5 MTU 1500; 6 full DHCP; 7 ARP client + DNS; 8 TCP echo) is a
separate build, net_<target>_rung<N>.hex, for each target part
(stc89c52rc, stc89c516rd), so a test runs against exactly the code that rung
adds, on every part it fits. A rung that does not fit a part has no hex and
its tests skip. The simulator's XRAM guard is set to each part's on-chip
XRAM, so a MOVX past it is a CPU exception (asserted absent in tearDown).

Negative controls (see 10_net_stack/NOTES.md):
  EMU8051_NET_RUNG_SHIFT=-1  run every rung's tests against the previous
                             rung's image: they must fail
  EMU8051_NET_XRAM=<bytes>   override the XRAM guard (e.g. 64): must fail Frames are built here with independently computed
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

# target part -> on-chip XRAM bytes (datasheet; see 10_net_stack/build.py)
TARGETS = {"stc89c52rc": 256, "stc89c516rd": 1024}
RUNG_SHIFT = int(os.environ.get("EMU8051_NET_RUNG_SHIFT", "0"))
XRAM_OVERRIDE = int(os.environ.get("EMU8051_NET_XRAM", "0"))

OUR_MAC = bytes.fromhex("020000000001")
OUR_IP = bytes([192, 168, 7, 2])        # NET_STATIC_IP, rungs 1-3
PEER_MAC = bytes.fromhex("a82bdd93cfef")
PEER_IP = bytes([192, 168, 7, 1])
BCAST_MAC = b"\xff" * 6
XID = bytes.fromhex("5ad10001")         # net.c
MTU = 576                               # rungs 1-4
MTU_BIG = 1500                          # rung 5+
PINS = dict(cs=(0, 3), sck=(0, 2), mosi=(0, 0), miso=(0, 1))


def hexfile(target, rung):
    return os.path.join(BUILD, f"net_{target}_rung{rung + RUNG_SHIFT}.hex")


def needs(target, rung):
    return unittest.skipUnless(
        os.path.isfile(hexfile(target, rung)),
        f"no {os.path.basename(hexfile(target, rung))}: not built, or the rung "
        f"does not fit {target} (see 10_net_stack/build.py output)")


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


def bootp_reply(msg_type, yiaddr, server, xid=XID, chaddr=OUR_MAC, extra_opts=b"",
                lease=3600, t1=None, t2=None, dns=None, router=None):
    fixed = struct.pack("!BBBB4sHH4s4s4s4s", 2, 1, 6, 0, xid, 0, 0x8000,
                        b"\0" * 4, yiaddr, server, b"\0" * 4)
    fixed += chaddr + b"\0" * 10 + b"\0" * 64 + b"\0" * 128
    opts = (bytes([99, 130, 83, 99]) + extra_opts + bytes([53, 1, msg_type]) +
            bytes([54, 4]) + server + bytes([1, 4, 255, 255, 255, 0]) +
            bytes([3, 4]) + (router or server))
    if lease is not None:
        opts += bytes([51, 4]) + struct.pack("!I", lease)
    if t1 is not None:
        opts += bytes([58, 4]) + struct.pack("!I", t1)
    if t2 is not None:
        opts += bytes([59, 4]) + struct.pack("!I", t2)
    if dns is not None:
        opts += bytes([6, 4]) + dns
    return fixed + opts + b"\xff"


def dhcp_frame(msg_type, yiaddr, server=PEER_IP, eth_dst=BCAST_MAC, ip_dst=b"\xff" * 4, **kw):
    bootp = bootp_reply(msg_type, yiaddr, server, **kw)
    u = udp(server, ip_dst, 67, 68, bootp)
    return eth(eth_dst, PEER_MAC, 0x0800, ipv4(server, ip_dst, 17, u))


TCP_FIN, TCP_SYN, TCP_RST, TCP_PSH, TCP_ACK = 0x01, 0x02, 0x04, 0x08, 0x10


def tcp(src_ip, dst_ip, sport, dport, seq, ack, flags, data=b"", opts=b"", window=8192,
        bad_sum=False):
    off = (20 + len(opts)) // 4
    hdr = struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF,
                      off << 4, flags, window, 0, 0) + opts
    pseudo = src_ip + dst_ip + struct.pack("!BBH", 0, 6, len(hdr) + len(data))
    s = csum(pseudo + hdr + data) ^ (0x5555 if bad_sum else 0)
    return hdr[:16] + struct.pack("!H", s) + hdr[18:] + data


def dns_name(name):
    return b"".join(bytes([len(l)]) + l.encode() for l in name.split(".")) + b"\0"


DNS_ID = 0xD5A1                          # net.c
DNS_QNAME = "time.example.com"           # netcfg.h NET_DNS_NAME


def dns_response(answer_ip, ident=DNS_ID, rcode=0, cname_first=True):
    """A realistic answer: question echoed, then (optionally) a CNAME whose
    owner is a compression pointer to the question, then the A record owned
    by a pointer into the CNAME's data."""
    q = dns_name(DNS_QNAME) + struct.pack("!HH", 1, 1)
    answers, an = b"", 0
    if rcode == 0:
        target = dns_name("ntp1.example.net")
        a_owner = b"\xc0\x0c"
        if cname_first:
            answers += b"\xc0\x0c" + struct.pack("!HHIH", 5, 1, 300, len(target)) + target
            a_owner = struct.pack("!H", 0xC000 | (12 + len(q) + 12))
            an += 1
        answers += a_owner + struct.pack("!HHIH", 1, 1, 300, 4) + answer_ip
        an += 1
    hdr = struct.pack("!HHHHHH", ident, 0x8180 | rcode, 1, an, 0, 0)
    return hdr + q + answers


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
            if self.proto == 6:
                (self.sport, self.dport, self.seq, self.ack, off, self.flags,
                 self.window) = struct.unpack("!HHIIBBH", self.l4[:16])
                self.tcp_hlen = (off >> 4) * 4
                self.tcp_opts = self.l4[20:self.tcp_hlen]
                self.data = self.l4[self.tcp_hlen:]


def assert_ip_ok(tc, p: Parsed):
    tc.assertEqual(p.etype, 0x0800)
    tc.assertEqual(p.ip[0], 0x45)
    tc.assertEqual(csum(p.ip), 0, "IP header checksum invalid")
    tc.assertLessEqual(14 + p.ip_total, len(p.raw))


# --- harness ---------------------------------------------------------------------

class Net:
    # Step size: also the resolution of every latency the tests print (1 ms).
    CHUNK = 1_000

    def __init__(self, target, rung):
        self.sim = Simulator("hc6800_es", hexfile(target, rung))
        self.sim.set_xram_size(XRAM_OVERRIDE or TARGETS[target])
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


class _Base:
    """Mixin: concrete unittest classes are generated per target part at the
    end of the module (see _instantiate)."""
    RUNG = None
    TARGET = None

    def setUp(self):
        self.net = Net(self.TARGET, self.RUNG)

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

class Rung2Icmp(_Base):
    RUNG = 2

    def test_ping_32_bytes(self):
        data = bytes(range(32))
        self.ping(data)
        (p,) = self.net.expect_tx()
        self.check_echo_reply(p, data)
        print(f"\n  [sim {self.TARGET}] 32 B ping latency: {self.net.last_latency_ticks / 1e3:.1f} ms")

    def test_ping_at_mtu(self):
        data = bytes((i * 7) & 0xFF for i in range(MTU - 28))   # 576 B datagram
        self.ping(data)
        (p,) = self.net.expect_tx(max_ticks=5_000_000)
        self.check_echo_reply(p, data)
        print(f"\n  [sim {self.TARGET}] {MTU} B ping latency: {self.net.last_latency_ticks / 1e3:.1f} ms")

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
        print(f"\n  [sim {self.TARGET}] DISCOVER retransmit interval: "
              f"{(self.net.sim.tick_count - first) / 1e6:.2f} s")


# --- shared: a bound DHCP client (rungs 5-8 inherit rung 4's tests too) --------

SEC = 1_000_000                         # sim ticks per second (12 MHz / 12T)


class _Bound(Rung4Dhcp):
    """Rung 4's DHCP tests are inherited, so every higher rung re-runs them
    as a regression check. New tests call bind() themselves; setUp does not,
    because the inherited tests need a fresh boot."""

    def bind(self, **opts):
        self.discover()
        self.assertEqual(self.net.inject(dhcp_frame(2, self.LEASE, **opts)),
                         Simulator.ENC_RX_OK)
        (req,) = self.net.expect_tx()
        self.check_dhcp_client_frame(req, 3)
        self.net.inject(dhcp_frame(5, self.LEASE, **opts))
        self.net.settle()
        self.bound_at = self.net.sim.tick_count

    def udp_to(self, ip, dport, data, sport=40000, with_sum=True):
        u = udp(PEER_IP, ip, sport, dport, data, with_sum)
        f = eth(OUR_MAC, PEER_MAC, 0x0800, ipv4(PEER_IP, ip, 17, u))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def check_udp_reply(self, p, sport, dport, data=None):
        assert_ip_ok(self, p)
        self.assertEqual((p.dst, p.src), (PEER_MAC, OUR_MAC))
        self.assertEqual((p.ip_src, p.ip_dst, p.proto), (self.LEASE, PEER_IP, 17))
        s, d, length, _ = struct.unpack("!HHHH", p.l4[:8])
        self.assertEqual((s, d, length), (sport, dport, len(p.l4)))
        pseudo = p.ip_src + p.ip_dst + struct.pack("!BBH", 0, 17, length)
        self.assertEqual(csum(pseudo + p.l4), 0, "UDP checksum invalid")
        if data is not None:
            self.assertEqual(p.l4[8:], data)
        return p.l4[8:]


# --- rung 5: MTU 1500 -----------------------------------------------------------

class Rung5Mtu(_Bound):
    RUNG = 5

    def test_ping_at_1500(self):
        self.bind()
        data = bytes((i * 11) & 0xFF for i in range(MTU_BIG - 28))
        self.ping(data, dst_ip=self.LEASE)
        (p,) = self.net.expect_tx(max_ticks=15 * SEC)
        self.check_echo_reply(p, data, src_ip=self.LEASE)
        print(f"\n  [sim {self.TARGET}] {MTU_BIG} B ping latency: "
              f"{self.net.last_latency_ticks / 1e3:.1f} ms")

    def test_udp_echo_at_1500(self):
        self.bind()
        data = bytes((i * 5) & 0xFF for i in range(MTU_BIG - 28))
        self.udp_to(self.LEASE, 7, data)
        (p,) = self.net.expect_tx(max_ticks=15 * SEC)
        self.check_udp_reply(p, 7, 40000, data)

    def test_above_1500_is_dropped(self):
        self.bind()
        self.ping(bytes(MTU_BIG - 28 + 2), dst_ip=self.LEASE)
        self.assertEqual(self.net.expect_silence(3 * SEC), [])

    def test_full_frames_across_ring_wrap(self):
        self.bind()
        for seq in range(1, 9):           # 8 x ~1.5 KB wraps the RX ring
            data = bytes((seq * 3 + i) & 0xFF for i in range(1400))
            self.ping(data, seq=seq, dst_ip=self.LEASE)
            (p,) = self.net.expect_tx(max_ticks=15 * SEC)
            self.check_echo_reply(p, data, seq=seq, src_ip=self.LEASE)


# --- rung 6: full DHCP (lease timer, renew, rebind, expiry) --------------------------

class Rung6Lease(_Bound):
    """Lease timers tick on a free-running 1 s clock (Timer 0, 20 x 50 ms),
    so a timer of T seconds fires between T-1 and T seconds after the ACK,
    depending on where in the second the ACK landed. RFC 2131's T1/T2 are
    advisory; the tests assert that window rather than an exact instant."""
    RUNG = 6

    def assert_fires(self, t, T):
        self.assertTrue(T - 1.05 < t <= T + 0.1, f"fired at {t:.2f} s, expected ({T - 1}, {T}]")

    def next_frame(self, max_s):
        (p,) = self.net.expect_tx(max_ticks=int(max_s * SEC))
        return p, (self.net.sim.tick_count - self.bound_at) / SEC

    def check_request_with_ciaddr(self, p, unicast):
        assert_ip_ok(self, p)
        self.assertEqual(p.src, OUR_MAC)
        self.assertEqual(p.dst, PEER_MAC if unicast else BCAST_MAC)
        self.assertEqual(p.ip_src, self.LEASE)
        self.assertEqual(p.ip_dst, PEER_IP if unicast else b"\xff" * 4)
        sport, dport = struct.unpack("!HH", p.l4[:4])
        self.assertEqual((sport, dport), (68, 67))
        bootp = p.l4[8:]
        self.assertEqual(bootp[4:8], XID)
        self.assertEqual(bootp[10:12], b"\0\0", "no broadcast flag once addressed")
        self.assertEqual(bootp[12:16], self.LEASE, "ciaddr")
        opts = self.options(bootp[240:])
        self.assertEqual(opts.get(53), bytes([3]))
        self.assertNotIn(50, opts, "RENEWING/REBINDING must not send option 50")
        self.assertNotIn(54, opts, "RENEWING/REBINDING must not send option 54")

    def test_renew_at_t1_is_unicast_with_ciaddr(self):
        self.bind(lease=8)
        p, t = self.next_frame(6)
        self.check_request_with_ciaddr(p, unicast=True)
        self.assert_fires(t, 4)
        print(f"\n  [sim {self.TARGET}] RENEW at {t:.2f} s of an 8 s lease")

    def test_ack_to_renew_restarts_the_timers(self):
        self.bind(lease=8)
        self.next_frame(6)                                   # RENEW at ~4 s
        self.net.inject(dhcp_frame(5, self.LEASE, eth_dst=OUR_MAC,
                                   ip_dst=self.LEASE, lease=8))
        self.net.settle()
        self.bound_at = self.net.sim.tick_count
        p, t = self.next_frame(6)                            # next RENEW, not REBIND
        self.check_request_with_ciaddr(p, unicast=True)
        self.assert_fires(t, 4)

    def test_unanswered_renew_rebinds_then_expires(self):
        self.bind(lease=8)
        p1, t1 = self.next_frame(6)
        self.check_request_with_ciaddr(p1, unicast=True)
        p2, t2 = self.next_frame(5)
        self.check_request_with_ciaddr(p2, unicast=False)     # REBIND: broadcast
        p3, t3 = self.next_frame(3)
        self.check_dhcp_client_frame(p3, 1)                   # expired: DISCOVER
        self.assert_fires(t1, 4)
        self.assert_fires(t2, 7)            # T2 = 0.875 * 8
        self.assert_fires(t3, 8)
        self.arp_request(self.LEASE)                          # address dropped
        self.assertEqual(self.net.expect_silence(), [])

    def test_explicit_t1_t2_options_are_used(self):
        self.bind(lease=100, t1=2, t2=3)
        p1, t1 = self.next_frame(4)
        self.check_request_with_ciaddr(p1, unicast=True)
        p2, t2 = self.next_frame(3)
        self.check_request_with_ciaddr(p2, unicast=False)
        self.assert_fires(t1, 2)
        self.assert_fires(t2, 3)

    def test_nak_while_renewing_drops_the_address(self):
        self.bind(lease=8)
        self.next_frame(6)                                    # RENEW
        self.net.inject(dhcp_frame(6, self.LEASE, eth_dst=OUR_MAC, ip_dst=self.LEASE))
        (p,) = self.net.expect_tx()
        self.check_dhcp_client_frame(p, 1)                    # straight to DISCOVER
        self.arp_request(self.LEASE)
        self.assertEqual(self.net.expect_silence(), [])

    def test_infinite_lease_never_renews(self):
        self.bind(lease=0xFFFFFFFF)
        self.assertEqual(self.net.expect_silence(8 * SEC), [])
        self.arp_request(self.LEASE)
        (a,) = self.net.expect_tx()
        self.check_arp_reply(a, ip=self.LEASE)


# --- rung 7: ARP client + DNS ------------------------------------------------------------

ROUTER_IP = bytes([192, 168, 7, 254])
ROUTER_MAC = bytes.fromhex("02aabbccdd01")
OFFLINK_DNS = bytes([8, 8, 8, 8])
RESOLVED = bytes([93, 184, 216, 34])
DNS_SPORT = 50000                        # netcfg.h NET_DNS_PORT
STATUS_PORT = 7777
DNS_ARP, DNS_QUERY, DNS_DONE, DNS_FAILED = 1, 2, 3, 4


class Rung7Dns(_Bound):
    RUNG = 7

    def expect_arp_query(self, target):
        (p,) = self.net.expect_tx()
        self.assertEqual(p.etype, 0x0806)
        self.assertEqual((p.dst, p.src), (BCAST_MAC, OUR_MAC))
        a = p.body
        self.assertEqual(struct.unpack("!HHBBH", a[:8]), (1, 0x0800, 6, 4, 1))
        self.assertEqual(a[8:14], OUR_MAC)
        self.assertEqual(a[14:18], self.LEASE)
        self.assertEqual(a[24:28], target)
        return p

    def answer_arp(self, ip, mac):
        f = eth(OUR_MAC, mac, 0x0806, arp(2, mac, ip, OUR_MAC, self.LEASE))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def expect_dns_query(self, hop_mac, server):
        (p,) = self.net.expect_tx()
        assert_ip_ok(self, p)
        self.assertEqual((p.dst, p.src), (hop_mac, OUR_MAC))
        self.assertEqual((p.ip_src, p.ip_dst, p.proto), (self.LEASE, server, 17))
        sport, dport, length, _ = struct.unpack("!HHHH", p.l4[:8])
        self.assertEqual((sport, dport, length), (DNS_SPORT, 53, len(p.l4)))
        pseudo = p.ip_src + p.ip_dst + struct.pack("!BBH", 0, 17, length)
        self.assertEqual(csum(pseudo + p.l4), 0, "UDP checksum invalid")
        m = p.l4[8:]
        self.assertEqual(struct.unpack("!HHHHHH", m[:12]), (DNS_ID, 0x0100, 1, 0, 0, 0))
        self.assertEqual(m[12:], dns_name(DNS_QNAME) + struct.pack("!HH", 1, 1))
        return p

    def answer_dns(self, msg, server, mac):
        u = udp(server, self.LEASE, 53, DNS_SPORT, msg)
        f = eth(OUR_MAC, mac, 0x0800, ipv4(server, self.LEASE, 17, u))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def status(self):
        self.udp_to(self.LEASE, STATUS_PORT, b"?", sport=40001)
        (p,) = self.net.expect_tx()
        data = self.check_udp_reply(p, STATUS_PORT, 40001)
        self.assertEqual(len(data), 5)
        return data[:4], data[4]

    def resolve_onlink(self, msg):
        self.bind(dns=PEER_IP)
        self.expect_arp_query(PEER_IP)
        self.answer_arp(PEER_IP, PEER_MAC)
        self.expect_dns_query(PEER_MAC, PEER_IP)
        self.answer_dns(msg, PEER_IP, PEER_MAC)
        self.net.settle()

    def test_resolves_via_onlink_server(self):
        self.resolve_onlink(dns_response(RESOLVED))
        self.assertEqual(self.status(), (RESOLVED, DNS_DONE))

    def test_resolves_a_record_without_cname(self):
        self.resolve_onlink(dns_response(RESOLVED, cname_first=False))
        self.assertEqual(self.status(), (RESOLVED, DNS_DONE))

    def test_offlink_server_goes_via_router(self):
        self.bind(dns=OFFLINK_DNS, router=ROUTER_IP)
        self.expect_arp_query(ROUTER_IP)
        self.answer_arp(ROUTER_IP, ROUTER_MAC)
        self.expect_dns_query(ROUTER_MAC, OFFLINK_DNS)
        self.answer_dns(dns_response(RESOLVED), OFFLINK_DNS, ROUTER_MAC)
        self.net.settle()
        self.assertEqual(self.status(), (RESOLVED, DNS_DONE))

    def test_nxdomain_reports_failure(self):
        self.resolve_onlink(dns_response(RESOLVED, rcode=3))
        self.assertEqual(self.status(), (b"\0" * 4, DNS_FAILED))

    def test_wrong_id_is_ignored_and_query_retried(self):
        self.bind(dns=PEER_IP)
        self.expect_arp_query(PEER_IP)
        self.answer_arp(PEER_IP, PEER_MAC)
        self.expect_dns_query(PEER_MAC, PEER_IP)
        t0 = self.net.sim.tick_count
        self.answer_dns(dns_response(RESOLVED, ident=0x1234), PEER_IP, PEER_MAC)
        self.net.settle()
        self.assertEqual(self.status(), (b"\0" * 4, DNS_QUERY))
        self.net.expect_tx(max_ticks=4 * SEC)                 # the retry ...
        dt = (self.net.sim.tick_count - t0) / SEC             # 2 s on a 1 s clock
        self.assertTrue(0.95 < dt <= 2.1, f"retried after {dt:.2f} s")
        self.answer_dns(dns_response(RESOLVED), PEER_IP, PEER_MAC)
        self.net.settle()
        self.assertEqual(self.status(), (RESOLVED, DNS_DONE))  # ... answered

    def test_unanswered_arp_gives_up(self):
        self.bind(dns=PEER_IP)
        for _ in range(5):                                    # NET_ARP_TRIES
            self.expect_arp_query(PEER_IP)
        self.assertEqual(self.net.expect_silence(int(1.5 * SEC)), [])
        self.assertEqual(self.status(), (b"\0" * 4, DNS_FAILED))


# --- rung 8: TCP echo ---------------------------------------------------------------------

PEER_PORT = 40000
MSS = MTU_BIG - 40


class Rung8Tcp(_Bound):
    RUNG = 8

    def seg(self, seq, ack, flags, data=b"", sport=PEER_PORT, dport=7, bad_sum=False,
            opts=b""):
        f = eth(OUR_MAC, PEER_MAC, 0x0800,
                ipv4(PEER_IP, self.LEASE, 6,
                     tcp(PEER_IP, self.LEASE, sport, dport, seq, ack, flags, data,
                         opts=opts, bad_sum=bad_sum)))
        self.assertEqual(self.net.inject(f), Simulator.ENC_RX_OK)

    def check_tcp(self, p, flags, seq=None, ack=None, data=b"", dport=PEER_PORT, sport=7):
        assert_ip_ok(self, p)
        self.assertEqual((p.dst, p.src), (PEER_MAC, OUR_MAC))
        self.assertEqual((p.ip_src, p.ip_dst, p.proto), (self.LEASE, PEER_IP, 6))
        pseudo = p.ip_src + p.ip_dst + struct.pack("!BBH", 0, 6, len(p.l4))
        self.assertEqual(csum(pseudo + p.l4), 0, "TCP checksum invalid")
        self.assertEqual((p.sport, p.dport), (sport, dport))
        self.assertEqual(p.flags, flags, f"flags {p.flags:#04x} != {flags:#04x}")
        if seq is not None:
            self.assertEqual(p.seq, seq & 0xFFFFFFFF)
        if ack is not None:
            self.assertEqual(p.ack, ack & 0xFFFFFFFF)
        self.assertEqual(p.data, data)

    def connect(self, cisn=1000):
        self.bind()
        self.seg(cisn, 0, TCP_SYN, opts=struct.pack("!BBH", 2, 4, 1460))
        (sa,) = self.net.expect_tx()
        self.check_tcp(sa, TCP_SYN | TCP_ACK, ack=cisn + 1)
        self.seg(cisn + 1, sa.seq + 1, TCP_ACK)
        self.net.settle()
        self.assertEqual(self.net.expect_silence(), [])
        return cisn + 1, sa.seq + 1                           # our seq, their seq

    def test_syn_ack_carries_mss_and_window(self):
        self.bind()
        self.seg(5000, 0, TCP_SYN)
        (sa,) = self.net.expect_tx()
        self.check_tcp(sa, TCP_SYN | TCP_ACK, ack=5001)
        self.assertEqual(sa.tcp_opts, struct.pack("!BBH", 2, 4, MSS))
        self.assertEqual(sa.window, MSS)

    def test_data_is_echoed(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH, b"hello, tcp")
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_ACK | TCP_PSH, seq=s, ack=c + 10, data=b"hello, tcp")

    def test_piggybacked_ack_with_new_data(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH, b"one")
        (e1,) = self.net.expect_tx()
        self.check_tcp(e1, TCP_ACK | TCP_PSH, seq=s, ack=c + 3, data=b"one")
        self.seg(c + 3, s + 3, TCP_ACK | TCP_PSH, b"two!")     # acks e1 and sends more
        (e2,) = self.net.expect_tx()
        self.check_tcp(e2, TCP_ACK | TCP_PSH, seq=s + 3, ack=c + 7, data=b"two!")

    def test_full_mss_segment_is_echoed(self):
        c, s = self.connect()
        data = bytes((i * 13) & 0xFF for i in range(MSS))
        self.seg(c, s, TCP_ACK | TCP_PSH, data)
        (p,) = self.net.expect_tx(max_ticks=15 * SEC)
        self.check_tcp(p, TCP_ACK | TCP_PSH, seq=s, ack=c + MSS, data=data)
        print(f"\n  [sim {self.TARGET}] {MSS} B TCP echo latency: "
              f"{self.net.last_latency_ticks / 1e3:.1f} ms")

    def test_peer_close_then_new_connection(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_FIN)
        (f,) = self.net.expect_tx()
        self.check_tcp(f, TCP_ACK | TCP_FIN, seq=s, ack=c + 1)
        self.seg(c + 1, s + 1, TCP_ACK)
        self.net.settle()
        self.assertEqual(self.net.expect_silence(), [])
        self.seg(9000, 0, TCP_SYN)                             # listening again
        (sa,) = self.net.expect_tx()
        self.check_tcp(sa, TCP_SYN | TCP_ACK, ack=9001)

    def test_data_with_fin_is_echoed_with_fin(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH | TCP_FIN, b"bye")
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_ACK | TCP_PSH | TCP_FIN, seq=s, ack=c + 4, data=b"bye")

    def test_syn_ack_retransmitted_then_reset(self):
        self.bind()
        self.seg(7000, 0, TCP_SYN)
        (first,) = self.net.expect_tx()
        t0 = self.net.sim.tick_count
        times = []
        for _ in range(5):                                     # NET_TCP_RETRIES
            (again,) = self.net.expect_tx(max_ticks=2 * SEC)
            self.assertEqual(again.raw, first.raw, "retransmission must be identical")
            times.append((self.net.sim.tick_count - t0) / SEC)
        (rst,) = self.net.expect_tx(max_ticks=2 * SEC)
        self.check_tcp(rst, TCP_RST | TCP_ACK, seq=first.seq + 1, ack=7001)
        for i, t in enumerate(times):
            self.assertAlmostEqual(t, i + 1.0, delta=0.15)     # RTO 1 s
        self.seg(7100, 0, TCP_SYN)                             # back to LISTEN
        (sa,) = self.net.expect_tx()
        self.check_tcp(sa, TCP_SYN | TCP_ACK, ack=7101)

    def test_unacked_echo_is_retransmitted_until_acked(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH, b"keep me")
        (e,) = self.net.expect_tx()
        (again,) = self.net.expect_tx(max_ticks=2 * SEC)
        self.assertEqual(again.raw, e.raw)
        self.seg(c + 7, s + 7, TCP_ACK)
        self.net.settle()
        self.assertEqual(self.net.expect_silence(3 * SEC), [])

    def test_bad_checksum_is_ignored(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH, b"corrupt", bad_sum=True)
        self.assertEqual(self.net.expect_silence(), [])
        self.seg(c, s, TCP_ACK | TCP_PSH, b"good")
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_ACK | TCP_PSH, seq=s, ack=c + 4, data=b"good")

    def test_duplicate_segment_gets_a_pure_ack(self):
        c, s = self.connect()
        self.seg(c, s, TCP_ACK | TCP_PSH, b"abc")
        self.net.expect_tx()
        self.seg(c + 3, s + 3, TCP_ACK)                        # ack our echo
        self.net.settle()
        self.seg(c, s + 3, TCP_ACK | TCP_PSH, b"abc")          # retransmitted by peer
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_ACK, seq=s + 3, ack=c + 3)

    def test_syn_to_closed_port_is_reset(self):
        self.bind()
        self.seg(3000, 0, TCP_SYN, dport=23)
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_RST | TCP_ACK, seq=0, ack=3001, sport=23)

    def test_second_peer_is_refused_while_busy(self):
        self.connect()
        self.seg(4000, 0, TCP_SYN, sport=40099)
        (p,) = self.net.expect_tx()
        self.check_tcp(p, TCP_RST | TCP_ACK, seq=0, ack=4001, dport=40099)

    def test_peer_reset_returns_to_listen(self):
        c, s = self.connect()
        self.seg(c, s, TCP_RST)
        self.net.settle()
        self.seg(8000, 0, TCP_SYN)
        (sa,) = self.net.expect_tx()
        self.check_tcp(sa, TCP_SYN | TCP_ACK, ack=8001)


# --- one concrete unittest class per (rung, target) --------------------------------

def _instantiate():
    for target in TARGETS:
        for cls in (Rung1Arp, Rung2Icmp, Rung3Udp, Rung4Dhcp,
                    Rung5Mtu, Rung6Lease, Rung7Dns, Rung8Tcp):
            name = f"{cls.__name__}_{target}"
            concrete = type(name, (cls, unittest.TestCase),
                            {"TARGET": target, "__module__": __name__})
            globals()[name] = needs(target, cls.RUNG)(concrete)


_instantiate()


if __name__ == "__main__":
    unittest.main()
