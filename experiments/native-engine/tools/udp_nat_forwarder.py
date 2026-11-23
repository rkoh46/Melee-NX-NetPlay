#!/usr/bin/env python3
"""Imitates a published container port for local tests: whatever arrives on PUBLIC_PORT is passed on to
127.0.0.1:TARGET_PORT from a fresh socket per sender, so the target sees each sender under a source
port it was never told about (as a container behind Docker's port publishing does). Replies to that
socket go back to the sender from PUBLIC_PORT. Loopback only.

usage: udp_nat_forwarder.py PUBLIC_PORT TARGET_PORT"""
# SPDX-License-Identifier: GPL-3.0-or-later
import select, socket, sys

public_port, target_port = int(sys.argv[1]), int(sys.argv[2])
public = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); public.bind(('127.0.0.1', public_port))
flows, back = {}, {}          # sender -> its socket toward the target; that socket -> sender
while True:
    ready, _, _ = select.select([public] + list(back), [], [], 1.0)
    for s in ready:
        try: data, sender = s.recvfrom(65536)
        except OSError: continue
        if s is public:
            if sender not in flows:
                flow = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); flow.bind(('127.0.0.1', 0))
                flows[sender] = flow; back[flow] = sender
                print('forwarder: sender port %d appears to the target as port %d' % (sender[1], flow.getsockname()[1]), flush=True)
            flows[sender].sendto(data, ('127.0.0.1', target_port))
        else:
            public.sendto(data, back[s])
