#!/usr/bin/env python3
"""Checks a spectator stream against a replay: the bytes a spectator client received for one game (spectator_client's
game-N.bin) must parse as Slippi events (the 0x35 command table first, every event the size it declares, Game End
last) and be the .slp's raw element byte for byte. A .slpz is unpacked by the caller first (or pass the .slp).

usage: spectator_check.py STREAM.bin REPLAY.slp"""
# SPDX-License-Identifier: GPL-3.0-or-later
import sys
from collections import Counter

def raw_element(slp):
    tag = b'raw[$U#l'
    at = slp.find(tag)
    if at < 0: raise SystemExit('no raw element in the replay')
    length = int.from_bytes(slp[at + 8:at + 12], 'big')
    return slp[at + 12:at + 12 + length]

def parse(events):
    if not events or events[0] != 0x35: raise SystemExit('the stream does not start with the command table (0x35)')
    n = events[1]
    sizes = {events[2 + 3 * i]: int.from_bytes(events[3 + 3 * i:5 + 3 * i], 'big') for i in range((n - 1) // 3)}
    at, counts, last = 1 + n, Counter(), None
    while at < len(events):
        c = events[at]
        if c not in sizes: raise SystemExit(f'event {c:02X} at byte {at} is not in the command table')
        at += 1 + sizes[c]; counts[c] += 1; last = c
    if at != len(events): raise SystemExit(f'the last event runs {at - len(events)} bytes past the end')
    return counts, last

stream = open(sys.argv[1], 'rb').read()
slp = open(sys.argv[2], 'rb').read()
counts, last = parse(stream)
names = {0x36: 'Game Start', 0x37: 'pre-frame', 0x38: 'post-frame', 0x39: 'Game End', 0x3A: 'frame start', 0x3B: 'item',
         0x3C: 'frame bookend', 0x10: 'split message'}
print('stream: %d bytes, %s; last event %s' % (len(stream), ', '.join(f'{counts[c]} {names.get(c, hex(c))}' for c in sorted(counts)),
                                                names.get(last, hex(last or 0))))
raw = raw_element(slp)
if stream == raw:
    print(f'stream == the replay\'s raw element ({len(raw)} bytes): IDENTICAL')
else:
    first = next((i for i in range(min(len(stream), len(raw))) if stream[i] != raw[i]), min(len(stream), len(raw)))
    print(f'stream ({len(stream)} bytes) and raw element ({len(raw)} bytes) DIFFER from byte {first}')
    sys.exit(1)
