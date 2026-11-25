"""Lab 12.2: the engine event hash (FNV-1a 64-bit), by hand.

include/lob/events.hpp turns every event into bytes and mixes them into one 64-bit number.
Two runs with the same events give the same number; change one field anywhere and the number
changes completely. This file does the same thing in Python for the events of the worked example
(SPEC.md section 1) and prints the hash. The playground (lab02_book_repl) prints the same
number when you type `hash` after the worked example.

Run:  python3 course/labs/py/fnv1a.py
"""

OFFSET = 0xCBF29CE484222325
PRIME = 0x100000001B3
MASK = (1 << 64) - 1


def fnv1a(data, h=OFFSET):
    for byte in data:
        h ^= byte              # mix the byte in...
        h = (h * PRIME) & MASK  # ...then stir (and keep 64 bits)
    return h


def u8(v):
    return bytes([v & 0xFF])


def u32(v):
    return (v & 0xFFFFFFFF).to_bytes(4, "little")


def u64(v):
    return (v & MASK).to_bytes(8, "little")  # negative int64 values wrap like the C++ cast


# The encodings of D-015: a tag byte, then every field in declaration order.
BUY, SELL = 0, 1


def ack(order_id, owner, client_order_id):
    return u8(0) + u64(order_id) + u32(owner) + u64(client_order_id)


def trade(trade_id, price, qty, maker_id, taker_id, maker_owner, taker_owner, aggressor):
    return (u8(2) + u64(trade_id) + u64(price) + u64(qty) + u64(maker_id) + u64(taker_id)
            + u32(maker_owner) + u32(taker_owner) + u8(aggressor))


if __name__ == "__main__":
    print("FNV-1a of the bytes of 'a':", hex(fnv1a(b"a")), "(the published test value is 0xaf63dc4c8601ec8c)")

    # The worked example as the playground runs it (client_order_id counts 1, 2, 3, 4).
    events = [
        ack(1, 1, 1),
        ack(2, 2, 2),
        ack(3, 3, 3),
        ack(4, 9, 4),
        trade(1, 101, 5, 1, 4, 1, 9, BUY),
        trade(2, 101, 3, 2, 4, 2, 9, BUY),
    ]
    h = OFFSET
    for e in events:
        h = fnv1a(e, h)
    print("hash of the worked example's six events:", format(h, "x"))

    # One field different: the second trade prints at 102 instead of 101.
    events[5] = trade(2, 102, 3, 2, 4, 2, 9, BUY)
    h2 = OFFSET
    for e in events:
        h2 = fnv1a(e, h2)
    print("same events, one price changed:        ", format(h2, "x"))
