#!/usr/bin/env python3
"""Prints the default gateway of the machine (or container) it runs in, from the kernel's routing table.
usage: default_gateway.py [route file, default /proc/net/route]"""
# SPDX-License-Identifier: GPL-3.0-or-later
import socket, struct, sys
for line in open(sys.argv[1] if len(sys.argv) > 1 else '/proc/net/route').read().splitlines()[1:]:
    field = line.split()
    # Destination 0.0.0.0 with the gateway flag: the address is stored as little-endian hexadecimal.
    if len(field) > 3 and field[1] == '00000000' and int(field[3], 16) & 2:
        print(socket.inet_ntoa(struct.pack('<I', int(field[2], 16))))
        break
