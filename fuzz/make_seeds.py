#!/usr/bin/env python3
"""Writes the seed corpus for the differential fuzzer, one file per scenario.

    python3 fuzz/make_seeds.py [--out DIR]     write the seeds (default fuzz/corpus/diff)
    python3 fuzz/make_seeds.py --check DIR     exit 1 if a seed in DIR is missing or differs

Every file is a sequence of 8-byte records in the format of the fuzz driver (fuzz/diff_driver.hpp):

    b0 kind % 3 (0 New, 1 Cancel, 2 Modify)   b1 owner - 1 (owner = 1 + b1 % 3)
    b2 flags: bit0 side (1 Sell), bit1 post_only, bit2 STP (1 CancelResting),
              bits3-4 type (0 Limit/GTC, 1 Limit/IOC, 2 Limit/FOK, 3 Market/IOC),
              bit5 raw_price, bit6 raw_qty, bit7 bad_tif
    b3,b4 p16, little-endian int16: price = p16 if raw_price else 95 + (uint16)p16 % 11
    b5    qty = int8(b5) if raw_qty else 1 + b5 % 10
    b6,b7 t, little-endian uint16: Cancel/Modify target is t if t % 8 == 7 or no id is known,
          else the (t % number of Acks so far)-th Ack id

The output depends on nothing but this file: no random module, no clock, no environment.
"""
import argparse
import struct
import sys
from pathlib import Path

NEW, CANCEL, MODIFY = 0, 1, 2
BUY, SELL = 0, 1
GTC, IOC, FOK, MARKET = 0, 1, 2, 3  # bits 3-4 of the flags
TAKING, RESTING = 0, 1              # STP: CancelTaking, CancelResting
UNKNOWN_ID_T = 7 + 8 * 100          # t % 8 == 7: used as the id itself; no scenario has 807 orders

DEFAULT_OUT = Path(__file__).resolve().parent / "corpus" / "diff"


def pack(kind, owner, flags, p16, b5, t):
    return struct.pack("<BBBhBH", kind, owner - 1, flags, p16, b5, t)


def price_fields(price, p16):
    """(raw_price flag bit, p16). Prices 95..105 use the band; any other value is sent raw."""
    if p16 is not None:
        return 0, p16
    if 95 <= price <= 105:
        return 0, price - 95
    return 0x20, price


def qty_fields(qty):
    """(raw_qty flag bit, b5 as an unsigned byte). Quantities 1..10 use the band."""
    if 1 <= qty <= 10:
        return 0, qty - 1
    return 0x40, qty & 0xFF


class Scenario:
    """Builds one input. `acks` counts the Acks the books will have emitted so far: the Nth
    accepted NewOrder gets OrderId N, so `ref(n)` can aim a Cancel/Modify at it."""

    def __init__(self):
        self.records = []
        self.acks = 0

    def new(self, owner, side, price, qty, typ=GTC, post_only=False, stp=TAKING,
            accepted=True, bad_tif=False, p16=None):
        raw_p, p = price_fields(price, p16)
        raw_q, q = qty_fields(qty)
        flags = (side | (0x02 if post_only else 0) | (0x04 if stp == RESTING else 0) |
                 (typ << 3) | raw_p | raw_q | (0x80 if bad_tif else 0))
        self.records.append(pack(NEW, owner, flags, p, q, 0))
        if accepted:
            self.acks += 1
        return self.acks  # the id of this order if it was accepted

    def target(self, wanted):
        """A t with t % 8 != 7 and t % acks == wanted - 1: the decoder resolves it to id `wanted`."""
        assert 1 <= wanted <= self.acks
        t = wanted - 1
        while t % 8 == 7:
            t += self.acks
        assert t < 65536 and t % self.acks == wanted - 1
        return t

    def cancel(self, owner, wanted=None):
        """Cancel order id `wanted`, or an id that no book knows when it is None."""
        t = UNKNOWN_ID_T if wanted is None else self.target(wanted)
        self.records.append(pack(CANCEL, owner, 0, 0, 0, t))

    def modify(self, owner, wanted, price, qty):
        t = UNKNOWN_ID_T if wanted is None else self.target(wanted)
        raw_p, p = price_fields(price, None)
        raw_q, q = qty_fields(qty)
        self.records.append(pack(MODIFY, owner, raw_p | raw_q, p, q, t))

    def data(self):
        return b"".join(self.records)


def worked_example():
    # SPEC upgrade 1 section 1, byte for byte (p16 = 100, so the price is 95 + 100 % 11 = 96).
    s = Scenario()
    s.new(1, BUY, 96, 6, p16=100)
    s.new(2, SELL, 96, 4, p16=100)
    return s.data()


def trade_at_maker_price():
    s = Scenario()
    s.new(1, SELL, 100, 5)
    s.new(2, BUY, 102, 2)   # trades 2 at 100, the maker's price, not at 102
    return s.data()


def fifo_one_level():
    s = Scenario()
    s.new(1, SELL, 100, 2)
    s.new(2, SELL, 100, 2)
    s.new(3, BUY, 100, 3)   # 2 from the first order, then 1 from the second
    return s.data()


def ioc_remainder():
    s = Scenario()
    s.new(1, SELL, 100, 2)
    s.new(2, BUY, 100, 5, typ=IOC)  # trades 2, the other 3 are cancelled and never rest
    return s.data()


def market_empty_side():
    s = Scenario()
    s.new(1, BUY, 100, 3, typ=MARKET)
    s.new(2, SELL, 100, 3, typ=MARKET)
    return s.data()


def _two_asks(s):
    s.new(1, SELL, 100, 2)
    s.new(2, SELL, 101, 3)


def fok_exact_fill():
    s = Scenario()
    _two_asks(s)
    s.new(3, BUY, 101, 5, typ=FOK)  # exactly the 5 lots on offer: fills
    return s.data()


def fok_one_lot_short():
    s = Scenario()
    _two_asks(s)
    s.new(3, BUY, 101, 6, typ=FOK)  # one lot more than on offer: cancelled, no trade
    return s.data()


def post_only_reject():
    s = Scenario()
    s.new(1, SELL, 100, 3)
    s.new(2, BUY, 100, 2, post_only=True, accepted=False)  # would cross: rejected
    s.new(2, BUY, 99, 2, post_only=True)                    # does not cross: rests
    return s.data()


def _two_bids(s):
    a = s.new(1, BUY, 100, 5)
    s.new(2, BUY, 100, 3)
    return a


def modify_reduce_keeps_priority():
    s = Scenario()
    first = _two_bids(s)
    s.modify(1, first, 100, 2)        # same price, smaller: keeps its place
    s.new(3, SELL, 100, 1)            # trades against the first order
    return s.data()


def modify_increase_loses_priority():
    s = Scenario()
    first = _two_bids(s)
    s.modify(1, first, 100, 8)        # larger: goes to the back of the level
    s.new(3, SELL, 100, 1)            # trades against the second order
    return s.data()


def modify_to_crossing_price():
    s = Scenario()
    s.new(1, SELL, 100, 3)
    bid = s.new(2, BUY, 98, 2)
    s.modify(2, bid, 100, 2)          # re-enters as a taker and trades 2 at 100
    return s.data()


def cancel_unknown_id():
    s = Scenario()
    s.new(1, BUY, 100, 2)
    s.cancel(1, None)                 # never existed
    s.cancel(1, 1)                    # works
    s.cancel(1, 1)                    # already gone: the id is known to the decoder, not to the book
    return s.data()


def cancel_wrong_owner():
    s = Scenario()
    s.new(1, BUY, 100, 2)
    s.cancel(2, 1)                    # not the owner: rejected, the order stays
    s.cancel(1, 1)
    return s.data()


def stp_cancel_taking():
    s = Scenario()
    s.new(1, SELL, 100, 3)
    s.new(1, BUY, 100, 2, stp=TAKING)  # would hit its own order: the incoming order is cancelled
    return s.data()


def _stp_cancel_resting_stream(s, typ):
    s.new(1, SELL, 100, 3)
    s.new(2, SELL, 100, 2)
    s.new(1, BUY, 100, 4, typ=typ, stp=RESTING)


def stp_cancel_resting():
    s = Scenario()
    _stp_cancel_resting_stream(s, GTC)  # owner 1's resting sell is cancelled, then trades with owner 2
    return s.data()


def _fok_stp(s, mode):
    s.new(1, SELL, 100, 3)            # own order, first in the queue
    s.new(2, SELL, 100, 3)
    s.new(1, BUY, 100, 3, typ=FOK, stp=mode)


def fok_stp_cancel_taking():
    s = Scenario()
    _fok_stp(s, TAKING)
    return s.data()


def fok_stp_cancel_resting():
    s = Scenario()
    _fok_stp(s, RESTING)
    return s.data()


def every_reject_reason():
    s = Scenario()
    bid = s.new(1, BUY, 100, 5)
    s.new(1, BUY, 100, 0, accepted=False)                       # InvalidQty
    s.new(1, BUY, 100, -3, accepted=False)                      # InvalidQty (negative)
    s.new(1, BUY, 0, 2, accepted=False)                         # InvalidPrice (zero)
    s.new(1, BUY, 5000, 2, accepted=False)                      # InvalidPrice (above max_price 1000)
    s.new(1, BUY, 100, 2, typ=MARKET, bad_tif=True, accepted=False)  # InvalidFlags (Market with GTC)
    s.new(1, BUY, 100, 2, typ=IOC, post_only=True, accepted=False)   # InvalidFlags (post-only IOC)
    s.cancel(1, None)                                           # UnknownOrder
    s.modify(1, None, 100, 2)                                   # UnknownOrder
    s.cancel(2, bid)                                            # NotOwner
    s.modify(2, bid, 99, 2)                                     # NotOwner
    s.modify(1, bid, 100, 5)                                    # NoChange
    s.modify(1, bid, 100, 0)                                    # InvalidQty
    s.modify(1, bid, 0, 5)                                      # InvalidPrice
    s.new(2, SELL, 101, 2)
    s.new(1, BUY, 101, 2, post_only=True, accepted=False)       # PostOnlyWouldCross (new order)
    quiet = s.new(1, BUY, 99, 2, post_only=True)
    s.modify(1, quiet, 101, 2)                                  # PostOnlyWouldCross (modify)
    return s.data()


class SplitMix64:
    MASK = (1 << 64) - 1

    def __init__(self, seed):
        self.state = seed & self.MASK

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & self.MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & self.MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & self.MASK
        return z ^ (z >> 31)


def mixed_stream_64():
    """64 requests from SplitMix64(1): mostly banded New orders, with Cancel and Modify aimed by
    a random t, and about one record in eight carrying a raw price or quantity."""
    rng = SplitMix64(1)
    out = []
    for _ in range(64):
        r = rng.next()
        pick = r & 0xFF
        kind = NEW if pick < 150 else (CANCEL if pick < 200 else MODIFY)
        owner = 1 + (r >> 8) % 3
        typ = (r >> 12) % 8
        typ = GTC if typ < 4 else (IOC if typ < 6 else (FOK if typ == 6 else MARKET))
        flags = ((r >> 16) & 1) | (0x02 if (r >> 17) % 10 == 0 else 0) | (0x04 if (r >> 18) & 1 else 0)
        flags |= typ << 3
        if (r >> 20) % 8 == 0:
            flags |= 0x20 | 0x40
        if (r >> 24) % 16 == 0:
            flags |= 0x80
        if kind != NEW:
            flags &= 0x60
        p16 = (r >> 28) & 0xFFFF
        if flags & 0x20:
            p16 = (p16 % 1200) - 100          # -100 .. 1099: below 1 and above max_price both occur
        else:
            p16 = p16 % 11
        b5 = (r >> 44) & 0xFF
        if flags & 0x40:
            b5 = (b5 % 14) - 2 & 0xFF         # -2 .. 11
        t = (r >> 48) & 0xFFFF
        out.append(pack(kind, owner, flags, p16, b5, t))
    return b"".join(out)


SCENARIOS = {
    "worked_example": worked_example,
    "trade_at_maker_price": trade_at_maker_price,
    "fifo_one_level": fifo_one_level,
    "ioc_remainder": ioc_remainder,
    "market_empty_side": market_empty_side,
    "fok_exact_fill": fok_exact_fill,
    "fok_one_lot_short": fok_one_lot_short,
    "post_only_reject": post_only_reject,
    "modify_reduce_keeps_priority": modify_reduce_keeps_priority,
    "modify_increase_loses_priority": modify_increase_loses_priority,
    "modify_to_crossing_price": modify_to_crossing_price,
    "cancel_unknown_id": cancel_unknown_id,
    "cancel_wrong_owner": cancel_wrong_owner,
    "stp_cancel_taking": stp_cancel_taking,
    "stp_cancel_resting": stp_cancel_resting,
    "fok_stp_cancel_taking": fok_stp_cancel_taking,
    "fok_stp_cancel_resting": fok_stp_cancel_resting,
    "every_reject_reason": every_reject_reason,
    "mixed_stream_64": mixed_stream_64,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--out", type=Path, default=DEFAULT_OUT, help="directory to write the seeds into")
    group.add_argument("--check", type=Path, metavar="DIR",
                       help="compare the seeds with the files in DIR; exit 1 on a missing or different file")
    args = ap.parse_args()

    seeds = {name + ".bin": fn() for name, fn in SCENARIOS.items()}
    if args.check:
        bad = 0
        for fname, data in seeds.items():
            path = args.check / fname
            if not path.is_file():
                print(f"missing seed: {path}", file=sys.stderr)
                bad += 1
            elif path.read_bytes() != data:
                print(f"seed differs from make_seeds.py output: {path}", file=sys.stderr)
                bad += 1
        if bad:
            print("run: python3 fuzz/make_seeds.py", file=sys.stderr)
            return 1
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    for fname, data in seeds.items():
        (args.out / fname).write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
