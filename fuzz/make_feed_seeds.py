#!/usr/bin/env python3
"""Writes the seed corpus of fuzz_feed_parse (upgrade 6): ITCH 5.0 feed files.

    python3 fuzz/make_feed_seeds.py [--out DIR]          the golden messages (FEED.md, section 3)
    python3 fuzz/make_feed_seeds.py --slice FEED [--out DIR]
                                                         also the first 2,000 messages of FEED

The golden seeds are the byte arrays of tests/unit/feed_itch_test.cpp, one message per file, plus
one valid feed that holds all five (S O, A, E, X, D, S C). The slice is cut at a message boundary
from a closed-loop feed; FEED.md gives the commands that made the committed one. Every multi-byte
field is big-endian, and each message is a 2-byte length followed by its body.
"""
import argparse
import struct
import sys
from pathlib import Path

OPEN_NS = 34_200_000_000_000  # 9:30:00


def prefix(kind, sim_ns):
    ts = OPEN_NS + sim_ns
    return kind.encode() + struct.pack(">HH", 1, 0) + ts.to_bytes(6, "big")


def framed(body):
    return struct.pack(">H", len(body)) + body


def msg_s(code, sim_ns):
    return framed(prefix("S", sim_ns) + code.encode())


def msg_a(ref, side, shares, ticks, sim_ns):
    return framed(prefix("A", sim_ns) + struct.pack(">Q", ref) + side.encode() + struct.pack(">I", shares)
                  + b"LOBSIM  " + struct.pack(">I", ticks * 100))


def msg_e(ref, shares, match, sim_ns):
    return framed(prefix("E", sim_ns) + struct.pack(">QIQ", ref, shares, match))


def msg_x(ref, shares, sim_ns):
    return framed(prefix("X", sim_ns) + struct.pack(">QI", ref, shares))


def msg_d(ref, sim_ns):
    return framed(prefix("D", sim_ns) + struct.pack(">Q", ref))


def golden():
    s_open = msg_s("O", 0)
    a = msg_a(7, "B", 5, 10_000, 1_500_000_000)
    e = msg_e(7, 3, 42, 2_000_000_000)
    x = msg_x(7, 2, 2_500_000_000)
    d = msg_d(7, 3_000_000_000)
    s_close = msg_s("C", 3_000_000_000)
    assert len(a) == 38 and len(e) == 33 and len(x) == 25 and len(d) == 21 and len(s_open) == 14
    # The section 3 hand check of FEED.md: the 38-byte A of order 7.
    assert a.hex() == "002441000100001f1b28421f0000000000000000074200000005" "4c4f4253494d2020000f4240"
    return {
        "golden_s_open.itch": s_open,
        "golden_a.itch": a,
        "golden_e.itch": e,
        "golden_x.itch": x,
        "golden_d.itch": d,
        "golden_feed.itch": s_open + msg_a(7, "B", 5, 10_000, 1_500_000_000) + e + msg_x(7, 1, 2_500_000_000) + d
        + s_close,
    }


def first_messages(data, count):
    at = 0
    for _ in range(count):
        if at + 2 > len(data):
            break
        (n,) = struct.unpack_from(">H", data, at)
        if at + 2 + n > len(data):
            break
        at += 2 + n
    return data[:at]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "corpus" / "feed_parse"))
    ap.add_argument("--slice", help="a feed file whose first 2,000 messages become closed_loop_2000.itch")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    seeds = golden()
    if args.slice:
        seeds["closed_loop_2000.itch"] = first_messages(Path(args.slice).read_bytes(), 2000)
    for name, data in seeds.items():
        (out / name).write_bytes(data)
        print(f"{out / name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
