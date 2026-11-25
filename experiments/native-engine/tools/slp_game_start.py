#!/usr/bin/env python3
"""Prints what a test needs from a Slippi replay's Game Start and stored Gecko code list: the replay version, the stage, the
isFrozenPS byte and the UCF entries of the code list (their injection address and size: UCF 0.84 brings its own entries such
as the pad buffer at 8006B460, UCF 0.8's dashback and shield drop are 352 and 312 bytes). Test aid only.

usage: slp_game_start.py FILE.slp"""
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys

data = open(sys.argv[1], 'rb').read()
head = data.index(b'raw[$U#l') + len(b'raw[$U#l')
length = struct.unpack('>i', data[head:head + 4])[0]
raw = data[head + 4:head + 4 + length]
if raw[0] != 0x35: sys.exit('not a Slippi replay: no Event Payloads first')
sizes = {raw[2 + 3 * k]: struct.unpack('>H', raw[3 + 3 * k:5 + 3 * k])[0] for k in range((raw[1] - 1) // 3)}
p, start, codes = 1 + raw[1], None, bytearray()
while p < len(raw):
    command = raw[p]
    if command not in sizes: sys.exit('unknown event %02X at %d' % (command, p))
    event = raw[p:p + 1 + sizes[command]]
    if command == 0x36 and start is None: start = event
    if command == 0x10 and event[1 + 514] == 0x3D:      # Message Splitter: 512 bytes, their count, the inner command
        n = struct.unpack('>H', event[1 + 512:1 + 514])[0]
        codes += event[1:1 + n]
    p += len(event)
if start is None: sys.exit('no Game Start')
version = '%d.%d.%d' % (start[1], start[2], start[3])
stage = struct.unpack('>H', start[0x13:0x15])[0]
frozen = start[0x1A2] if len(start) > 0x1A2 else None
ucf_sites = {0x800D65EC: 'squat', 0x800C9A44: 'dashback', 0x8006B460: 'pad buffer', 0x8008E54C: 'SDI', 0x8009A0B8: 'shield drop ext',
             0x800998A4: 'shield drop', 0x80093294: 'shield SDI', 0x800908F4: 'tumble', 0x802662D0: 'text'}
ucf, i = [], 0
while i + 8 <= len(codes) and codes[i] != 0xFF:
    kind = codes[i] & 0xFE
    address = (struct.unpack('>I', codes[i:i + 4])[0] & 0x01FFFFFF) | 0x80000000
    word = struct.unpack('>I', codes[i + 4:i + 8])[0]
    n = 8 + word * 8 if kind in (0xC0, 0xC2) else 16 if kind == 0x08 else 8 + ((word + 7) & ~7) if kind == 0x06 else 8
    if address in ucf_sites: ucf.append('%s %d' % (ucf_sites[address], n))
    i += n
print('version %s, stage %d, isFrozenPS %s, code list %d bytes, UCF entries: %s' %
      (version, stage, frozen, len(codes), ', '.join(ucf) if ucf else 'none'))
