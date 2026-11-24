#!/usr/bin/env python3
"""Writes a copy of a Slippi replay without its stored Gecko code list, as replays recorded before Slippi stored one look
(the playback then uses Slippi's legacy list: frontend/src/features/replays.cpp, slippi_playback_legacy.h).

The code list travels in Message Splitter events (0x10) whose internal command is 0x3D; those are dropped and the raw
element's length is corrected. Everything else, the metadata included, is copied unchanged. Test data only.

usage: slp_strip_codes.py IN.slp OUT.slp"""
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
head = data.index(b'raw[$U#l') + len(b'raw[$U#l')
length = struct.unpack('>i', data[head:head + 4])[0]
raw = data[head + 4:head + 4 + length]
if raw[0] != 0x35: sys.exit('not a Slippi replay: no Event Payloads first')
sizes = {raw[2 + 3 * k]: struct.unpack('>H', raw[3 + 3 * k:5 + 3 * k])[0] for k in range((raw[1] - 1) // 3)}
out, p, dropped = bytearray(raw[:1 + raw[1]]), 1 + raw[1], 0
while p < len(raw):
    command = raw[p]
    if command not in sizes: sys.exit('unknown event %02X at %d' % (command, p))
    event = raw[p:p + 1 + sizes[command]]
    if command == 0x10 and event[1 + 514] == 0x3D: dropped += 1
    else: out += event
    p += len(event)
open(dst, 'wb').write(data[:head] + struct.pack('>i', len(out)) + bytes(out) + data[head + 4 + length:])
print('%s: %d code list messages dropped, raw %d -> %d bytes' % (dst, dropped, length, len(out)))
