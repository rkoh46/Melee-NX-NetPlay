#!/usr/bin/env python3
# Test aid: a Falco costume skin made from the player's own disc (never committed): the disc's PlFcNr.dat with the root's first
# child joint moved by DELTA units (default 5: melee-unlocked's online rule turns it off; 1e-4 stays inside its tolerance).
#   make_test_skin.py GALE01.iso OUT.dat [DELTA]
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys
iso, out = sys.argv[1], sys.argv[2]
delta = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
f = open(iso, "rb"); f.seek(0x424); fst_off, fst_size = struct.unpack(">II", f.read(8)); f.seek(fst_off); fst = f.read(fst_size)
count = struct.unpack(">I", fst[8:12])[0]; names = count * 12
dat = None
for i in range(1, count):
    e = fst[i*12:i*12+12]
    if e[0]: continue
    n = fst[names + (struct.unpack(">I", e[0:4])[0] & 0xFFFFFF):].split(b"\0")[0].decode()
    if n.lower() == "plfcnr.dat":
        off, size = struct.unpack(">II", e[4:12]); f.seek(off); dat = bytearray(f.read(size)); break
if dat is None: sys.exit("PlFcNr.dat not on the disc")
dsize, nrel, nroot, nref = struct.unpack(">IIII", dat[4:20])
table = 0x20 + dsize + nrel * 4; strings = table + (nroot + nref) * 8
for r in range(nroot):
    j, name_at = struct.unpack(">II", dat[table + r*8: table + r*8 + 8])
    if dat[strings + name_at:].split(b"\0")[0].endswith(b"_Share_joint"): root = j; break
child = struct.unpack(">I", dat[0x20 + root + 8: 0x20 + root + 12])[0]
at = 0x20 + child + 0x2C                                    # the joint's rest translation x
x = struct.unpack(">f", dat[at:at+4])[0]; dat[at:at+4] = struct.pack(">f", x + delta)
open(out, "wb").write(bytes(dat))
