#!/usr/bin/env python3
# Test aid for tools/ciso_gcz_check.sh: a made-up disc image and its CISO and GCZ forms (the layouts Dolphin reads; see
# frontend/src/disc_ciso_gcz.cpp). No game data.
# SPDX-License-Identifier: GPL-3.0-or-later
import os, random, struct, sys, zlib
out = sys.argv[1]
rng = random.Random(7)
size = 48 << 20
img = bytearray(size)
img[0:6] = b"GALE01"
for start in range(1 << 20, size, 3 << 20):                       # random stretches and repeated text, gaps of zeros
    n = min(1 << 20, size - start)
    img[start:start + n] = bytes(rng.getrandbits(8) for _ in range(4096)) * (n // 4096) if (start >> 20) % 2 else (b"Melee test " * (n // 11 + 1))[:n]
open(f"{out}/disc.iso", "wb").write(img)
# CISO: 2 MiB blocks, the map in a 0x8000-byte header, empty blocks left out.
block = 2 << 20
with open(f"{out}/disc.ciso", "wb") as f:
    header = bytearray(0x8000); header[0:4] = b"CISO"; header[4:8] = struct.pack("<I", block)
    body = []
    for i in range(size // block):
        chunk = img[i * block:(i + 1) * block]
        if any(chunk) or i == size // block - 1:
            header[8 + i] = 1; body.append(chunk)
    f.write(header); [f.write(c) for c in body]
# GCZ: 32 KiB blocks, zlib, a block stored when it does not shrink.
block = 32 << 10
count = size // block
offsets, hashes, data = [], [], bytearray()
for i in range(count):
    chunk = bytes(img[i * block:(i + 1) * block])
    z = zlib.compress(chunk, 6)
    stored = len(z) >= len(chunk)
    payload = chunk if stored else z
    offsets.append(len(data) | (1 << 63 if stored else 0)); hashes.append(zlib.adler32(payload) & 0xFFFFFFFF); data += payload
def gcz(path, d):
    with open(path, "wb") as f:
        f.write(struct.pack("<IIQQII", 0xB10BC001, 0, len(d), size, block, count))
        f.write(b"".join(struct.pack("<Q", o) for o in offsets)); f.write(b"".join(struct.pack("<I", h) for h in hashes)); f.write(d)
gcz(f"{out}/disc.gcz", data)
bad = bytearray(data); bad[len(bad) // 2] ^= 0x40
gcz(f"{out}/bad.gcz", bad)
print("made disc.iso (%d bytes), disc.ciso, disc.gcz (%d bytes of blocks), bad.gcz" % (size, len(data)))
