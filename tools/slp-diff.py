#!/usr/bin/env python3
"""Compares two Slippi replays of the same deterministic match, frame by frame.

Pre-frame records carry the inputs and the RNG seed, post-frame records the fighter state
(position, action, percent, shield, ...). Two builds that run the game identically write the
same records; the first difference names the frame, the player and the field offset.
Usage: slp-diff.py a.slp b.slp
"""
import struct
import sys

PRE, POST = 0x37, 0x38


def records(path):
    data = open(path, "rb").read()
    i = data.index(b"raw[$U#l") + 8
    n = struct.unpack(">I", data[i:i + 4])[0]
    raw = data[i + 4:i + 4 + n]
    assert raw[0] == 0x35, "no event payload table"
    size = raw[1]
    sizes = {raw[2 + k * 3]: struct.unpack(">H", raw[3 + k * 3:5 + k * 3])[0] for k in range((size - 1) // 3)}
    out = {}
    p = 1 + size
    while p < len(raw):
        cmd = raw[p]
        ln = sizes.get(cmd)
        if ln is None:
            break
        body = raw[p + 1:p + 1 + ln]
        if cmd in (PRE, POST):
            frame, player, follower = struct.unpack(">iBB", body[:6])
            out[(frame, player, follower, cmd)] = body
        p += 1 + ln
    return out


a, b = records(sys.argv[1]), records(sys.argv[2])
keys = sorted(set(a) & set(b))
diffs = [k for k in keys if a[k] != b[k]]
frames = sorted({k[0] for k in keys})
print(f"{len(keys)} records in both ({len(frames)} frames, {frames[0]}..{frames[-1]}); "
      f"only in A {len(set(a) - set(b))}, only in B {len(set(b) - set(a))}; differing {len(diffs)}")
for k in diffs[:10]:
    x, y = a[k], b[k]
    off = next(i for i in range(min(len(x), len(y))) if x[i] != y[i])
    print(f"  frame {k[0]} player {k[1]} {'pre' if k[3] == PRE else 'post'} byte {off + 1}: "
          f"{x[off:off + 8].hex()} / {y[off:off + 8].hex()}")
sys.exit(1 if diffs else 0)
