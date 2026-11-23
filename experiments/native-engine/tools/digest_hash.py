#!/usr/bin/env python3
"""FNV-1a 64 of a state digest CSV, as mu-native prints it. usage: digest_hash.py file.csv [...]"""
# SPDX-License-Identifier: GPL-3.0-or-later
import sys
for path in sys.argv[1:]:
    h = 0xcbf29ce484222325
    for b in open(path, 'rb').read():
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    print('%016x  %s' % (h, path))
