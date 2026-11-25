#!/usr/bin/env python3
"""Turns a copy of the player's own disc image into a small mod disc with one alternate stage, for the held-button alternate
stage test (frontend/test-mods-linux.sh alt-stage; frontend/src/features/cosmetics.cpp, melee-unlocked's
host/cosmetic_mods.cpp): Battlefield's file GrNBa.dat copied with the image data of its textures inverted (visibly different,
structurally the same file) and added to the disc as "GrNBa.lat", the name a 20XX-style disc gives the stage's L alternate.

The copy (DISC, modified in place: make it with `cp -c` on APFS so that only the changed blocks take space) gets the new file and
a new file table at its end; the header's file table offset and size point at the new table. Nothing from the disc is ever
committed: the test makes the copy from the player's disc in the test's output folder.

usage: make_alt_stage_disc.py DISC"""
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys

BPP = {0: 4, 1: 8, 2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 8: 4, 9: 8, 10: 16, 14: 4}   # GX texture formats
BLOCK = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 8: (8, 8), 9: (8, 4), 10: (4, 4), 14: (8, 8)}

def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def be16(b, o): return struct.unpack_from('>H', b, o)[0]

def invert_textures(dat):
    """The image data of every texture the file's image descriptions point at, inverted. An image description is a relocated
    pointer to the pixels followed by width, height and format; the pixels hold no pointers, so changing them cannot break the
    file. Returns the new bytes and how many images and bytes were changed."""
    data_size, reloc_count = be32(dat, 4), be32(dat, 8)
    data = bytearray(dat[0x20:0x20 + data_size])
    relocs = sorted(be32(dat, 0x20 + data_size + 4 * i) for i in range(reloc_count))
    reloc_set = set(relocs)
    seen, changed = set(), 0
    for r in relocs:
        if r + 12 > data_size or (r + 4) in reloc_set or (r + 8) in reloc_set: continue
        w, h, fmt = be16(data, r + 4), be16(data, r + 6), be32(data, r + 8)
        if fmt not in BPP or not (8 <= w <= 1024 and 8 <= h <= 1024): continue
        bw, bh = BLOCK[fmt]
        size = ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * BPP[fmt] // 8
        target = be32(data, r)
        if target in seen or target + size > data_size: continue
        if any(target <= x < target + size for x in relocs): continue   # pointers inside: not pixels
        seen.add(target)
        for i in range(target, target + size): data[i] ^= 0xFF
        changed += size
    return dat[:0x20] + bytes(data) + dat[0x20 + data_size:], len(seen), changed

disc = sys.argv[1]
with open(disc, 'r+b') as f:
    f.seek(0x424); fst_off, fst_size = struct.unpack('>II', f.read(8))
    f.seek(fst_off); fst = f.read(fst_size)
    n = be32(fst, 8)
    strings = fst[n * 12:]
    def name(i): o = be32(fst, i * 12) & 0xFFFFFF; return strings[o:strings.index(b'\0', o)].decode('latin1')
    names = {name(i).lower(): i for i in range(1, n) if not fst[i * 12]}
    if 'grnba.lat' in names: sys.exit('%s already has GrNBa.lat' % disc)
    i = names['grnba.dat']
    f.seek(be32(fst, i * 12 + 4)); original = f.read(be32(fst, i * 12 + 8))
    alternate, images, changed = invert_textures(original)
    if images == 0: sys.exit('no texture found in GrNBa.dat')
    f.seek(0, 2); end = f.tell()
    file_at = (end + 0x7FFF) & ~0x7FFF                     # a file starts on a 32 KiB boundary on Nintendo's discs
    f.seek(file_at); f.write(alternate)
    # The new table: every entry as it was, the new file appended to the root (no directory's range reaches past the old last
    # entry), the root's entry count raised, the name added at the end of the name table.
    entries = bytearray(fst[:n * 12])
    struct.pack_into('>I', entries, 8, n + 1)
    entries += struct.pack('>III', len(strings), file_at, len(alternate))
    table = bytes(entries) + strings + b'GrNBa.lat\0'
    table_at = (file_at + len(alternate) + 0x7FFF) & ~0x7FFF
    f.seek(table_at); f.write(table)
    f.seek(0x424); f.write(struct.pack('>III', table_at, len(table), len(table)))
print('%s: GrNBa.lat added (Battlefield, %d textures inverted, %d bytes of pixels), file table moved to %d' % (disc, images, changed, table_at))
